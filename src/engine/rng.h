#ifndef WOS_RNG_H
#define WOS_RNG_H
#include <stdint.h>
/* The original's rules RNG: MSVC 6 CRT rand() on the main thread.
 * holdrand = holdrand*214013 + 2531011; rand() = (holdrand >> 16) & 0x7fff.
 * Every rand() call the original makes, on any path (rules, AI, visuals), must be mirrored by
 * exactly one crt_rand() call in the same order, or all later outcomes diverge.
 *
 * This is the ONLY generator. The old `Rng`/`rng_seed`/`rng_next`/`rng_bounded`/`game_rng`
 * API (a splitmix64 that the original does not have) was removed at the cutover: grep for
 * rng_/game_rng returns nothing outside this header. */
void crt_srand(uint32_t seed);
int crt_rand(void); /* 0..32767 */
uint32_t crt_rand_state(void);
uint64_t crt_rand_calls(void);
/* Number of crt_srand() calls; the original makes 2 at boot (all.c:27953, 27987). */
uint64_t crt_srand_calls(void);
#endif
