#include "rng.h"
static Rng global_rng = { 1 };
void rng_seed(Rng *rng, uint32_t seed) { rng->state = seed; }
uint32_t rng_next(Rng *rng)
{
    uint32_t z = (rng->state += UINT32_C(0x9e3779b9));
    z = (z ^ (z >> 16)) * UINT32_C(0x21f0aaad);
    z = (z ^ (z >> 15)) * UINT32_C(0x735a2d97);
    return z ^ (z >> 15);
}
uint32_t rng_bounded(Rng *rng, uint32_t bound)
{
    uint32_t x, threshold;
    if (!bound) return 0;
    threshold = (uint32_t)(-bound) % bound;
    do { x = rng_next(rng); } while (x < threshold);
    return x % bound;
}
Rng *game_rng(void) { return &global_rng; }
