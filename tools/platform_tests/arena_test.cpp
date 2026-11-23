/* arena_test - the game heap's arena on POSIX
 * (src/platform/posix/heap_arena_posix.cpp):
 *   1. heap_arena reserves the size asked (arena_test <MB>: GT3PC_HEAP_MB's
 *      value, or 768, the default) ending at or below 0x80000000, zeroed and
 *      writable at both ends, and gives the same block again;
 *   2. the control: a block of that size left to the kernel's placement
 *      (mmap without an address) lands above 0x80000000 - the check of 1
 *      is not true of any block.
 * Exit code 0 when everything passes. */
#include "platform/heap_arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

static int g_failures;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        g_failures++;
}

int main(int argc, char **argv)
{
    const uint32_t mb = argc > 1 ? (uint32_t)atoi(argv[1]) : 768;
    uint32_t size = 0;
    uint8_t *p = (uint8_t *)heap_arena(&size);
    const uintptr_t base = (uintptr_t)p, end = base + size;
    printf("arena: base 0x%08X, %u MB, end 0x%08X\n", (unsigned)base, (unsigned)(size >> 20), (unsigned)end);
    check(p != nullptr && size == mb << 20, "the size asked");
    check(base < 0x80000000u && end <= 0x80000000u && end > base, "base and end at or below 0x80000000");
    bool zero = p != nullptr && p[0] == 0 && p[size - 1] == 0;
    if (p != nullptr) {
        p[0] = 1;
        p[size - 1] = 2;
    }
    check(zero && p[0] == 1 && p[size - 1] == 2, "zeroed and writable at both ends");
    uint32_t again = 0;
    check(heap_arena(&again) == p && again == size, "the same block on the next call");

    void *k = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("control: the kernel's own placement of %u MB: 0x%08X\n", (unsigned)(size >> 20),
           (unsigned)(uintptr_t)k);
    check(k != MAP_FAILED && (uintptr_t)k >= 0x80000000u, "control: left to the kernel it lands above 0x80000000");
    if (k != MAP_FAILED)
        munmap(k, size);

    printf("%s\n", g_failures == 0 ? "arena_test: all passed" : "arena_test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
