/* game.h - common base of the canonical game headers (include/game/).
 *
 * Every canonical header documents the ORIGINAL layout, the one SCE ee-gcc
 * 2.9-ee-991111b (egcs) produced: member offsets in comments, sizes through
 * EGCS_SIZE().  tools/hdrcheck.py computes each documented offset and size with
 * egcs and compares.  GCC 14 lays polymorphic classes and bool out differently
 * (vptr at +0 instead of after the first polymorphic base's data; bool is 1 byte,
 * egcs makes it 4), so the size checks are compiled only by egcs; a GCC 14 build is
 * consistent with itself.
 *
 * The headers are written in the C++ both compilers accept: egcs's C++98 dialect
 * (no override, = default, = delete, in-class member initialisers, enum class,
 * constexpr; static_assert only at namespace scope, through types.h). */
#ifndef GAME_GAME_H
#define GAME_GAME_H

#include "types.h"

/* PC: checked for the types whose layout does not depend on the compiler's
 * C++ ABI (no virtual functions): the plain records read from the game's
 * files and saves must keep the PS2 layout.  Polymorphic classes have
 * the PC compiler's own layout (vptr first).  Checked by every PC C++
 * compiler: MSVC, and GCC / Clang on i686, where types.h gives the 64-bit
 * types MSVC's 8-byte alignment and PC_TAIL_PAD keeps derived classes out
 * of their bases' tail padding. */
#if defined(__cplusplus) && (defined(_MSC_VER) || (defined(__GNUC__) && defined(__i386__)))
#include <type_traits>
#define EGCS_SIZE(T, n) \
    static_assert(std::is_polymorphic<T>::value || sizeof(T) == (n), "PS2 size of " #T)
#else
#define EGCS_SIZE(T, n) typedef char BOOT_CAT(egcs_size_unchecked_, __LINE__)
#endif
/* PC: the PS2 size of a plain type whose size still depends on the C++ ABI
 * (polymorphic members, pointers to members): not checked.  None of them is
 * read from a file. */
#define EGCS_SIZE_ABI(T, n) typedef char BOOT_CAT(egcs_size_abi_, __LINE__)

#endif /* GAME_GAME_H */
