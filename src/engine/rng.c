#include "rng.h"
/* MSVC 6 CRT rand (msvcrt holdrand starts at 1). */
static uint32_t crt_holdrand = 1;
static uint64_t crt_calls;
void crt_srand(uint32_t seed) { crt_holdrand = seed; }
int crt_rand(void)
{
    crt_holdrand = crt_holdrand * UINT32_C(214013) + UINT32_C(2531011);
    ++crt_calls;
    return (int)((crt_holdrand >> 16) & 0x7fff);
}
uint32_t crt_rand_state(void) { return crt_holdrand; }
uint64_t crt_rand_calls(void) { return crt_calls; }
