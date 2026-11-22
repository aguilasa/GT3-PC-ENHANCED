/* T414_race_random.cpp - the race random generator RaceRandom (? names)
 * (0x00211A00..0x00211B58).
 *
 * A T531 Random at gp-0x47A0 (g_00353250), seeded from the COP0 Random
 * register when the static constructor builds it.  The race code skips /
 * draws values from it; GameStatus (T429) saves its seed with the game.
 *
 * Retail T414 has no .rodata and no .sdata: the generator is in .sbss
 * (0x00353250, between T150's and T248's file statics), so it was a file
 * static (an initialised or public object would be .sdata).
 */
#include "game/race/core/race_input_internal.h"
#include "game/race/core/T414_race_random.h"              /* RaceRandom, raceRandom* */
#include "engine/pdistd/T531_random.h"                   /* Random (base of RaceRandom), random_int */

/* The generator (.sbss 0x00353250, gp-0x47A0): a file static with a
 * constructor (local, written after the last function).  Only T414's
 * functions use it; others go through raceRandom*. */
static RaceRandom g_00353250;                   /* 0x00353250 */

/* The COP0 Random register (the TLB replacement index, free-running). */
/* PC: no such register: a value in 0..47 (the EE's TLB indices) from the
 * CPU's time-stamp counter (docs/system.md 2.5).  The intrinsic is declared
 * here, not through <intrin.h> (game files include no compiler library
 * headers). */
#ifdef _MSC_VER
extern "C" unsigned __int64 __rdtsc(void);
#pragma intrinsic(__rdtsc)
#else
/* GCC / Clang: the intrinsic comes from <x86intrin.h>, whose <mm_malloc.h>
 * calls malloc / free: read it with the C library's names, not the
 * pdistd heap's (libc.h). */
#pragma push_macro("malloc")
#pragma push_macro("free")
#undef malloc
#undef free
#include <x86intrin.h>
#pragma pop_macro("free")
#pragma pop_macro("malloc")
#endif
static inline u32 cop0_random(void)
{
    return (u32)(__rdtsc() % 48);
}

// FUNC 0x00211A00 RaceRandom::RaceRandom
//   Random() stores the seed 0x87654321 first (overwritten by randomize).
RaceRandom::RaceRandom()
{
    initialSeed = 0;
    randomize();
}

// FUNC 0x00211A38 RaceRandom::randomize
//   Both words = COP0 Random, then one step (Random::next, F_0026C118).
void RaceRandom::randomize()
{
    u32 r = cop0_random();

    seed = r;
    initialSeed = r;
    next();
}

// FUNC 0x00211A68 raceRandomShuffle
//   Skips 1..12 values: (COP0 Random + random_int()) % 13 (signed), at
//   least 1.
void raceRandomShuffle(void)
{
    int mixed = (int)cop0_random();
    int count;

    mixed += random_int();
    count = mixed % 13;
    raceRandomSkip((count > 0) ? count : 1);
}

// FUNC 0x00211AC0 raceRandomSkip
void raceRandomSkip(int n)
{
    for (; n > 0; n--)
        g_00353250.next();
}

// FUNC 0x00211B00 raceRandomFloat
//   The n-th next value as a float in [0, 1) (Random::next01, F_0026C178).
f32 raceRandomFloat(int n)
{
    raceRandomSkip(n - 1);
    return g_00353250.next01();
}

// FUNC 0x00211B28 raceRandomGetSeed
u32 raceRandomGetSeed(void)
{
    return g_00353250.seed;
}

// FUNC 0x00211B30 raceRandomSetSeed
void raceRandomSetSeed(u32 seed)
{
    g_00353250.seed = seed;
}

/* Compiler-generated: egcs's static constructor of g_00353250 (.ctors entry
 * 0x0028F16C), RaceRandom() on it, emitted at the end of the unit. */
// FUNC 0x00211B38 T414_static_init (_GLOBAL_$I$__10RaceRandom)
/* implicit: RaceRandom (the constructor) */
