#include "encint.h"
#include "rng.h"
#include <stdio.h>
#include <string.h>

/* DAT_004E709C in the original: set by FUN_0049B665 when a checksum fails, and
 * read as the `== 0` guard on FUN_0040A7C7's 20 ms idle gate. A tampered object
 * therefore stops the game's tick entirely. */
static int cheat_flag;

/* FUN_0049B665. The original compares `(double)v * C` against the stored double
 * for each of the three, exactly; it never checks k0..k3 against anything. */
static int verify(const EncInt *e)
{
    double v = (double)e->v;
    if (v * ENCINT_C0 != e->d0 || v * ENCINT_C1 != e->d1 || v * ENCINT_C2 != e->d2) {
        cheat_flag = 1;
        return 0;
    }
    return 1;
}

/* FUN_0049B6C7: the three doubles first, from the CURRENT v, then four rand()
 * into k0, k1, k2, k3 in that order. The double writes happen before the draws,
 * so an object whose v is set and re-set in a row keeps only the last key set. */
static void reseal(EncInt *e)
{
    double v = (double)e->v;
    e->d0 = ENCINT_C0 * v;
    e->d1 = ENCINT_C1 * v;
    e->d2 = v * ENCINT_C2;
    e->k0 = crt_rand();
    e->k1 = crt_rand();
    e->k2 = crt_rand();
    e->k3 = crt_rand();
}

void enc_set(EncInt *e, int32_t value)
{
    verify(e);
    e->v = value;
    reseal(e);
}

void enc_clear(EncInt *e)
{
    e->v = 0;
    reseal(e);
}

int32_t enc_get(const EncInt *e)
{
    verify(e);
    return e->v;
}

int32_t enc_add(EncInt *e, int32_t delta)
{
    int32_t cur = enc_get(e);   /* a get, then a set: 4 rand() */
    enc_set(e, cur + delta);
    return e->v;
}

int32_t enc_raw(const EncInt *e) { return e->v; }
int enc_valid(const EncInt *e) { return verify(e); }
int enc_cheat_flag(void) { return cheat_flag; }
void enc_cheat_clear(void) { cheat_flag = 0; }

void enc_construct_array(void *base, size_t offset, size_t stride, int count)
{
    int i;
    unsigned char *p = (unsigned char *)base;
    if (!base || !stride) return;
    for (i = 0; i < count; ++i) enc_clear((EncInt *)(p + (size_t)i * stride + offset));
}

const char *enc_dump_keys(const EncInt *e, char *out, size_t cap)
{
    if (!out || !cap) return "";
    snprintf(out, cap, "%d:%d:%d:%d", (int)e->k0, (int)e->k1, (int)e->k2, (int)e->k3);
    return out;
}
