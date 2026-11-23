/* pgl_intrin.h - the compiler intrinsics the renderer uses, by their MSVC
 * names.
 *
 * Windows: <intrin.h>, nothing else.  GCC / Clang: <x86intrin.h> (__rdtsc,
 * _mm_pause) and the few MSVC-only ones on the builtins, with the same
 * results: _BitScanForward on __builtin_ctz, _InterlockedCompareExchange64
 * on __atomic_compare_exchange_n (cmpxchg8b on i686, sequentially
 * consistent as the interlocked calls are). */
#ifndef PGL_NATIVE_PGL_INTRIN_H
#define PGL_NATIVE_PGL_INTRIN_H

#ifdef _WIN32

#include <intrin.h>

#else /* !_WIN32 */

#include <x86intrin.h>

/* The lowest set bit of mask into *index: nonzero; 0 (and *index unchanged)
 * when mask is 0. */
static inline unsigned char _BitScanForward(unsigned long *index, unsigned long mask)
{
    if (mask == 0)
        return 0;
    *index = (unsigned long)__builtin_ctzl(mask);
    return 1;
}

/* *dest = exchange when it is comparand; the value it had, either way.  dest
 * 8-byte aligned, as the interlocked call requires (then cmpxchg8b inline,
 * not libatomic's call: a long long is 4-aligned in the i386 ABI). */
typedef long long pgl_interlocked64 __attribute__((aligned(8)));
static inline long long _InterlockedCompareExchange64(volatile long long *dest, long long exchange,
                                                      long long comparand)
{
    __atomic_compare_exchange_n((volatile pgl_interlocked64 *)dest, &comparand, exchange, false, __ATOMIC_SEQ_CST,
                                __ATOMIC_SEQ_CST);
    return comparand;
}

#endif /* _WIN32 */

#endif /* PGL_NATIVE_PGL_INTRIN_H */
