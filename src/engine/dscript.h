/* The .dsc diff script: a schedule of timed input against the virtual clock.
 *
 * One `at <ms>` line per scheduled event; `end <ms>` closes the run. Times are
 * virtual milliseconds from process start, so both sides of the differential
 * replay drive the same schedule. The Oracle owns the grammar in
 * docs/re/oracle.md; this is the port's parser and scheduler.
 *
 * Owner: Core. */
#ifndef WOS_DSCRIPT_H
#define WOS_DSCRIPT_H

#include <stddef.h>
#include <stdint.h>

#define DSCRIPT_OPS_MAX   4096
#define DSCRIPT_TEXT_MAX  1024
#define DSCRIPT_KV_MAX    16
#define DSCRIPT_KEYS_MAX  128

typedef enum {
    DS_CLICK,     /* x y button */
    DS_RCLICK,    /* x y */
    DS_DOWN,      /* x y button */
    DS_UP,        /* x y button */
    DS_MOVE,      /* x y */
    DS_KEY,       /* key */
    DS_TEXT,      /* text */
    DS_DIALOG,    /* id ctrl=value... ok|cancel */
    DS_DUMP,      /* label */
    DS_END        /* stop at this time */
} DscriptOpKind;

typedef struct {
    uint32_t at_ms;
    DscriptOpKind kind;
    int x, y, button, key, id;
    int control_count;
    int control[DSCRIPT_KV_MAX];   /* control ids, in script order */
    int value[DSCRIPT_KV_MAX];
    /* A text value (`1043=Walker`, an edit control); empty for a numeric one. The oracle
     * hook reads the same line, so `sel:N` and `check:N` are N here as they are there. */
    char svalue[DSCRIPT_KV_MAX][32];
    int ok;                        /* dialog: 1 = ok, 0 = cancel */
    char text[DSCRIPT_TEXT_MAX];   /* text payload, or the dump label */
} DscriptOp;

typedef struct {
    DscriptOp ops[DSCRIPT_OPS_MAX];
    size_t count;
    size_t pc;          /* next unconsumed op; dscript_take() advances it */
    uint32_t end_ms;
    int has_end;
} Dscript;

/* Borrows and mutates `text`. Returns 0, or -1 with the 1-based failing line. */
int dscript_parse(Dscript *ds, char *text, size_t *error_line);
/* Virtual ms of the next op at or after `from`, or UINT32_MAX when none is left. */
uint32_t dscript_next_time(const Dscript *ds, uint32_t from);
/* First op at `now_ms` that has not been consumed, or NULL. Ops are consumed in
 * file order, so a later line never pre-empts an earlier one at the same time. */
const DscriptOp *dscript_take(Dscript *ds, uint32_t now_ms);

#endif
