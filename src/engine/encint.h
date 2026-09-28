/* The original's anti-cheat "encrypted int" (the EncInt object).
 *
 * Layout, from FUN_0049B6C7's integer arithmetic, is 13 ints with the three
 * checksum doubles interleaved:
 *
 *   offset  0  int32_t  v      the value
 *   offset  4  int32_t  k0     random key, draw 1
 *   offset  8  double   d0     v * 2.1459
 *   offset 16  int32_t  k1     random key, draw 2
 *   offset 24  double   d1     v * 1.4142
 *   offset 32  int32_t  k2     random key, draw 3
 *   offset 40  double   d2     v * 0.0123
 *   offset 48  int32_t  k3     random key, draw 4
 *
 * The three multipliers are the .rdata doubles at VA 0x004D0D38, 0x004D0D40 and
 * 0x004D0D48. The keys are stored but never verified against anything: the
 * check (FUN_0049B665) only re-derives the three doubles from v and compares.
 * So k0..k3 exist to be part of the same memory pattern a cheat tool would have
 * to recognise, and they still cost four rand() per set.
 *
 * SIZE: the last field ends at byte 52, but the doubles make the struct 8-aligned, so
 * **sizeof(EncInt) is 56, not 52** (FrontHero-2 measured it; the stride between the ten
 * sealed objects in FUN_00413181 is 0x38 = 56, which confirms it against the binary).
 * Any struct embedding several EncInts carries the same 4 bytes of tail padding, so an
 * offset table computed by summing the fields above will be 4 bytes short per object.
 * The compile-time check at the bottom of this header is what keeps that honest.
 *
 * RANDOM COST, which is the part that matters for parity:
 *   enc_set   -> 4 crt_rand()   (FUN_0049B71B: verify, v = value, FUN_0049B6C7)
 *   enc_clear -> 4 crt_rand()   (FUN_0049B734: v = 0, FUN_0049B6C7)
 *   enc_add   -> 4 crt_rand()   (FUN_0049B73F: a get, then a set)
 *   enc_get   -> 0              (FUN_0049B70F)
 *
 * The 1408 rand() calls at boot are 352 of these being constructed
 * (FUN_0049B75D wraps FUN_0049B734 as the constructor), via FUN_00401125.
 *
 * On a checksum mismatch FUN_0049B665 sets DAT_004E709C and calls FUN_004A8664
 * (0x424) and FUN_00449A17 -- the cheat path. That flag is also the `== 0` guard
 * on FUN_0040A7C7's 20 ms gate, so a tampered value stops the game's tick
 * entirely. The port reports the failure rather than emulating the penalty.
 *
 * Owner: Core. */
#ifndef WOS_ENCINT_H
#define WOS_ENCINT_H

#include <stddef.h>
#include <stdint.h>

/* The three checksum multipliers, from VA 0x004D0D38 / 0x004D0D40 / 0x004D0D48. */
#define ENCINT_C0 2.1459
#define ENCINT_C1 1.4142
#define ENCINT_C2 0.0123

typedef struct EncInt {
    int32_t v;
    int32_t k0;
    double  d0;
    int32_t k1;
    double  d1;
    int32_t k2;
    double  d2;
    int32_t k3;
} EncInt;

/* All four consume exactly four crt_rand() in the order k0, k1, k2, k3. */
void    enc_set(EncInt *e, int32_t value);   /* FUN_0049B71B */
void    enc_clear(EncInt *e);                /* FUN_0049B734; the ctor is FUN_0049B75D */
int32_t enc_add(EncInt *e, int32_t delta);   /* FUN_0049B73F: a get then a set */
/* Consumes no rand. Verifies the three doubles and, on a mismatch, flags the
 * same cheat state the original does. */
int32_t enc_get(const EncInt *e);            /* FUN_0049B70F */
/* Non-verifying reads, for dumping and for code the original reads raw. */
int32_t enc_raw(const EncInt *e);
/* 1 while the object verifies, 0 once the cheat flag has been raised. */
int     enc_valid(const EncInt *e);
/* 1 once any object has failed to verify (the original's DAT_004E709C). */
int     enc_cheat_flag(void);
void    enc_cheat_clear(void);
/* The original's DAT_004E709C: also the `== 0` guard on the 20 ms idle gate. */
const char *enc_dump_keys(const EncInt *e, char *out, size_t cap);

/* sizeof is 56, not 52: see the layout note above. Anyone deriving an offset table from the
 * field offsets has to allow for the 4 bytes of tail padding. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(EncInt) == 56, "EncInt must be 56 bytes: 52 of fields plus 4 of tail padding");
#endif

/* MSVC's `eh vector constructor iterator` (FUN_00401125 and the thunks that call
 * it): construct `count` objects of `stride` bytes, each one consuming four
 * crt_rand(). This is how the original's boot arrays are built -- the array at VA
 * 0x0052C978 is 99 elements of 288 bytes (count 0x63, stride 0x120), built by the thunk
 * at 0x00401101.
 *
 * `base` is the start of the containing array, which is what the original's own struct
 * is: the 288-byte element holds the EncInt plus ~232 bytes of other fields, so pass
 * the containing type and the offset of its EncInt:
 *     enc_construct_array(&table, offsetof(Table, stat), sizeof(Table), n, ENC_ASCENDING);
 *
 * DIRECTION IS NOT A UNIVERSAL RULE, and getting it wrong shifts the whole session's
 * stream. The boot array thunk walks ASCENDING. But FUN_00413181 (the pet constructor)
 * walks each of its two five-object groups from the HIGHEST offset DOWNWARDS: the ECX
 * sequence is -0x128, -0xF0, -0xB8, -0x80, -0x48, then -0x808, -0x7D0, -0x798, -0x760,
 * -0x728, with a stride of 0x38 = sizeof(EncInt) inside each group. So the direction is
 * a property of the site, and the caller has to state it. FrontHero-2 found this by
 * reading the ECX sequence out of the disassembly. */
typedef enum { ENC_ASCENDING = 0, ENC_DESCENDING = 1 } EncOrder;
void enc_construct_array(void *base, size_t offset, size_t stride, int count, EncOrder order);

#endif
