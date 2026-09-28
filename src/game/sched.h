/* The scheduled-event table of FUN_00456B87 and the SRN boot mixer of FUN_0042B4E0.
 * Both run in offline solo play, so the port carries them whatever the code was
 * originally written for. Owner: Core. */
#ifndef WOS_SCHED_H
#define WOS_SCHED_H

#include <stddef.h>
#include <stdint.h>

/* FUN_00456B87's cap, and the count that lives at obj+8 in the original. */
#define SCHED_ENTRIES_MAX 128
/* 0x610 bytes per entry in the original; the port keeps only the four written fields. */
#define SCHED_ENTRY_STRIDE 0x610

typedef struct {
    int      id;         /* +0x0C */
    uint32_t tick_ms;    /* +0x20C, the GetTickCount stamp taken at append time */
    int      count;      /* +0x40C, the third argument */
    int      delay_ms;   /* +0x60C, (rand() % (count*2)) * 1000 */
    int      fired;      /* port-only bookkeeping for sched_due() */
} SchedEntry;

/* FUN_00456B87: append one entry, consuming exactly one crt_rand(). Returns the
 * new index, or -1 when the table is full (after which it consumes nothing more). */
int  sched_append(int id, int count);
/* FUN_00456C51: the four boot appends, ids 14..17, count 30. */
void sched_boot(void);
/* The consumer, FUN_00456EC1 via FUN_00456D2F. ITS TEST IS NOT YET READ -- see
 * docs/re/timing.md section 8.2b. What is here is the shape the writer implies. */
int  sched_due(int *fired, int max);
int  sched_count(void);
const SchedEntry *sched_at(int i);

/* FUN_0042B4E0: five crt_rand() plus clock_ms() per iteration, looping while the
 * fold is exactly zero. Five is the usual count, not a fixed one. */
void srn_mix(void);

#endif
