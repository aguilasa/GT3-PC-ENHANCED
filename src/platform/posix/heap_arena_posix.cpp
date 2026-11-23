/* heap_arena_posix.cpp - the memory of the game heap off Windows
 * (heap_arena.h).
 *
 * The same sizes and passes as on Windows: below 2 GB first, then anywhere.
 * A 32-bit process's mmap places blocks top-down from below the stack and
 * the libraries (0xF7..., on a 64-bit kernel), above 2 GB: the first pass
 * asks for fixed places from low addresses up (MAP_FIXED_NOREPLACE: a
 * place in use is refused, never replaced; a kernel older than 4.17 takes
 * it as a hint) and keeps a block only when it ends at or below 2 GB.  The
 * block is anonymous memory: zeroed, its pages given on first touch. */
#include "platform/heap_arena.h"
#include "platform/log.h"

#include <stdlib.h>
#include <sys/mman.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace {

constexpr uint32_t kSizesMb[] = {768, 512, 384, 256, 128};
constexpr uint32_t kEnvMinMb = 128, kEnvMaxMb = 2048;
constexpr uintptr_t kLowLimit = 0x80000000u;
/* The places tried in the first pass: from 256 MB (above a non-PIE
 * program's image at 0x08048000 and its brk heap's first growth) in 16 MB
 * steps. */
constexpr uintptr_t kLowFirst = 0x10000000u, kLowStep = 0x01000000u;
void *g_arena;
uint32_t g_size;

void *map_at(uintptr_t at, uint32_t bytes, int extra)
{
    void *p = mmap((void *)at, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | extra, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

void *reserve(uint32_t bytes, bool low)
{
    if (!low)
        return map_at(0, bytes, 0);
    for (uintptr_t at = kLowFirst; at + bytes <= kLowLimit; at += kLowStep) {
        void *p = map_at(at, bytes, MAP_FIXED_NOREPLACE);
        if (p == nullptr)
            continue;
        if ((uintptr_t)p + bytes <= kLowLimit && (uintptr_t)p + bytes > (uintptr_t)p)
            return p;
        munmap(p, bytes);                   /* (a hint not taken: it went high) */
    }
    return nullptr;
}

} // namespace

void *heap_arena(uint32_t *size)
{
    if (!g_arena) {
        uint32_t askedMb = 0;
        if (const char *e = getenv("GT3PC_HEAP_MB")) {
            const int v = atoi(e);
            askedMb = v < (int)kEnvMinMb ? kEnvMinMb : v > (int)kEnvMaxMb ? kEnvMaxMb : (uint32_t)v;
        }
        for (int pass = 0; pass < 2 && !g_arena; pass++) {
            if (askedMb != 0 && !g_arena) {
                g_arena = reserve(askedMb << 20, pass == 0);
                if (g_arena)
                    g_size = askedMb << 20;
            }
            for (uint32_t mb : kSizesMb) {
                if (g_arena)
                    break;
                g_arena = reserve(mb << 20, pass == 0);
                if (g_arena)
                    g_size = mb << 20;
            }
        }
        if (!g_arena)
            pclog::fatal("could not reserve the game heap (%u MB)", kSizesMb[sizeof(kSizesMb) / sizeof(kSizesMb[0]) - 1]);
        pclog::write("heap: %u MB at %p%s", g_size >> 20, g_arena,
                     askedMb != 0 ? (g_size == askedMb << 20 ? " (GT3PC_HEAP_MB)" : " (GT3PC_HEAP_MB's size could not be had)")
                                  : "");
    }
    *size = g_size;
    return g_arena;
}
