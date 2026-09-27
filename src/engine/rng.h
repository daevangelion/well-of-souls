#ifndef WOS_RNG_H
#define WOS_RNG_H
#include <stdint.h>
typedef struct { uint32_t state; } Rng;
void rng_seed(Rng *rng, uint32_t seed);
uint32_t rng_next(Rng *rng);
uint32_t rng_bounded(Rng *rng, uint32_t bound); /* 0 bound returns 0 */
Rng *game_rng(void);
#endif
