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

/* The hero block's placement offset, from FUN_004269AF (0x004269AF) at VA 0x00426B21.
 * Oracle3 and FrontHero-2 were right that the draw index is #1409, after the 352 EncInt
 * constructions; I initially took the draw from the wrong place in that function. The
 * disassembly is unambiguous:
 *
 *     0x00426B0E  call time      ; 0x00426B18  call srand     ; 0x00426B21  call rand
 *     0x00426B27  cltd / xor %edx,%eax / sub %edx,%eax   -> abs(draw)
 *     0x00426B31  and $0x3fff,%eax                        -> abs(draw) & 0x3FFF
 *     0x00426B3C  call 0x00426149 (malloc 0x15C0170)  ; 0x00426B47 -> DAT_004E486C
 *     0x00426B41  and $0xfffffff0,%edi                   -> & ~15
 *     0x00426B81  add 0x4e486c,%edi   ; 0x00426B94 -> DAT_004E4870 ; 0x00426B9A -> DAT_0067FBF8
 *
 * So DAT_004E4870 = (abs(draw) & 0x3FFF & ~15) + <heap block>, and the hero record sits
 * 0x1560A5C above that. The masked draw is 0..0x3FF0, always a multiple of 16.
 *
 * WHAT THIS RETURNS, and why: the masked draw only. The other term is a malloc'd heap
 * address, which differs per run in the original and is not reproducible, so the oracle
 * must be comparing the masked draw too -- publishing the sum would mismatch on every
 * run and read as an RNG divergence. Set by seed_crt(); reads 0 before boot. */
uint32_t crt_boot_base_offset(void);
void    crt_boot_base_offset_set(uint32_t value);
#endif
