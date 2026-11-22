/* libc.h - the C library as the game and the engine use it (GT3 PC).
 *
 * On the PS2, malloc / free / realloc / calloc / memalign were Polyphony's
 * pdistd allocator (engine/pdistd T532: one heap, 16-byte aligned blocks, a
 * memalign'd block released with plain free), and printf / vprintf /
 * sprintf / vsprintf pdistd's own format engine (T533..T535).  The port
 * keeps both (the heap over an arena the platform gives it); rand and qsort
 * are newlib's (src/platform/pc_libc.cpp).  The rest is the MSVC C runtime,
 * which has what the game calls (string, memory, ctype, sscanf, strtol...).
 * Only the game and engine's own files see these names (pc_prelude.h);
 * the platform code uses the C runtime's.
 *
 * The names are mapped with macros after the C runtime headers are in, so
 * the runtime's own declarations stay untouched. */
#ifndef BOOT_LIBC_H
#define BOOT_LIBC_H

#include "types.h"
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#ifndef _MSC_VER
/* GCC / Clang with glibc and libstdc++: pull in every header whose inline
 * code names a C library function (std::rand in <algorithm>, ::free in
 * <memory>, ::sinf in <cmath>...) before the macros below rename them, and
 * the fixed-width integer types MSVC's headers bring in on their own. */
#include <stdint.h>
#include <strings.h>
#ifdef __cplusplus
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <random>
#include <memory>
#include <string>
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

void  *pc_malloc(size_t size);                         /* pdistd T532 */
void   pc_free(void *ptr);                             /* pdistd T532 */
void  *pc_realloc(void *ptr, size_t size);             /* pdistd T532 */
void  *pc_calloc(size_t nmemb, size_t size);           /* pdistd T532 */
void  *pc_memalign(size_t align, size_t size);         /* pdistd T532 */
void   pc_qsort(void *a, size_t n, size_t es, int (*cmp)(const void *, const void *));
int    pc_printf(const char *fmt, ...);                /* pdistd T533 */
int    pc_vprintf(const char *fmt, va_list ap);        /* pdistd T533 */
int    pc_sprintf(char *buf, const char *fmt, ...);    /* pdistd T534 */
int    pc_vsprintf(char *buf, const char *fmt, va_list ap); /* pdistd T535 */
/* newlib's rand: the 64-bit LCG with RAND_MAX 0x7FFFFFFF (MSVC's rand has
 * RAND_MAX 0x7FFF, which the game's ranges do not expect). */
int    pc_rand(void);
void   pc_srand(unsigned int seed);
char  *pc_strupr(char *s);
float  pc_atan2f(float y, float x);                    /* pdistd fastmath */
float  pc_atanf(float x);
float  pc_cosf(float x);
float  pc_expf(float x);
float  pc_logf(float x);
float  pc_powf(float x, float y);
void   pc_sincosf(float x, float *s, float *c);
float  pc_sinf(float x);
float  pc_tanf(float x);

#ifdef __cplusplus
}
#endif

#define malloc      pc_malloc
#define free        pc_free
#define realloc     pc_realloc
#define calloc      pc_calloc
#define memalign    pc_memalign
#define printf      pc_printf
#define vprintf     pc_vprintf
#define sprintf     pc_sprintf
#define vsprintf    pc_vsprintf
#define rand        pc_rand
#define srand       pc_srand
#define strupr      pc_strupr
#define qsort       pc_qsort
/* Polyphony's single-precision math (engine/pdistd/fastmath.cpp): its
 * approximations, not the C runtime's results, drive the physics and the
 * cameras. */
#define atan2f      pc_atan2f
#define atanf       pc_atanf
#define cosf        pc_cosf
#define expf        pc_expf
#define logf        pc_logf
#define powf        pc_powf
#define sincosf     pc_sincosf
#define sinf        pc_sinf
#define tanf        pc_tanf
#undef  RAND_MAX
#define RAND_MAX    0x7FFFFFFF
#ifdef _MSC_VER
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#else
/* The MSVC runtime names the decompiled code and the port use, mapped to
 * their POSIX / GCC equivalents. */
#define _stricmp            strcasecmp
#define _strnicmp           strncasecmp
#define _byteswap_ulong(x)  __builtin_bswap32(x)
#endif

#endif /* BOOT_LIBC_H */
