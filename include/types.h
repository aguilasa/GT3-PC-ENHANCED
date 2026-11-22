/* types.h - the common types of the game and the engine (GT3 PC).
 *
 * The decomp's version described the PS2 EE (MIPS R5900: 32-bit pointers,
 * 64-bit GPRs, 128-bit lq / sq) for two compilers, the period egcs and GCC
 * 14.  The port compiles with MSVC for 32-bit x86: pointers stay 32 bits, so
 * the game's in-place relocated data keeps its layout; `long long` is the
 * 64-bit type as before (u64 / s64).  The 128-bit GPR type is a plain
 * 16-byte aligned pair of words (only packet code used it as a number). */
#ifndef BOOT_TYPES_H
#define BOOT_TYPES_H

#define BOOT_CAT_(a, b) a##b
#define BOOT_CAT(a, b)  BOOT_CAT_(a, b)

/* Attributes of the GCC builds that have no meaning here. */
#define BOOT_USED
#define BOOT_MAY_ALIAS

#include <math.h>

/* EE sqrt.s / abs.s (the decomp kept them as asm for matching). */
#define boot_sqrtf(x)    sqrtf(x)
#define boot_sqrtf_v(x)  sqrtf(x)
#define boot_fabsf(x)    fabsf((float)(x))

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
typedef float               f32;
#if !defined(_MSC_VER) && defined(__i386__)
/* The i386 SysV ABI aligns 64-bit members to 4 bytes inside a struct; MSVC
 * and the EE align them to 8.  The alignment goes on the types themselves
 * (not -malign-double, which also changes long double and the layout of the
 * C library's own structs), so every record holding them keeps the MSVC /
 * PS2 offsets and size. */
typedef unsigned long long  u64 __attribute__((aligned(8)));
typedef signed long long    s64 __attribute__((aligned(8)));
typedef double              f64 __attribute__((aligned(8)));
#else
typedef unsigned long long  u64;
typedef signed long long    s64;
typedef double              f64;
#endif

/* The Itanium C++ ABI (GCC / Clang on i386) puts a derived class's first
 * member or next base inside the tail padding of a base that is not POD;
 * egcs and MSVC start it past the base's whole size.  PC_TAIL_PAD fills that
 * padding with an unused member there, so the derived class keeps the MSVC /
 * PS2 offsets; it is empty for the other compilers. */
#if !defined(_MSC_VER) && defined(__i386__)
#define PC_TAIL_PAD(name, n) u8 name[n];
#else
#define PC_TAIL_PAD(name, n)
#endif

/* An EE 128-bit register value (a quadword in memory: lq / sq). */
#ifdef __cplusplus
struct alignas(16) u128 { u64 lo, hi; };
struct alignas(16) s128 { u64 lo, hi; };
#else
typedef struct { _Alignas(16) u64 lo; u64 hi; } u128;
typedef struct { _Alignas(16) u64 lo; u64 hi; } s128;
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif

#endif /* BOOT_TYPES_H */
