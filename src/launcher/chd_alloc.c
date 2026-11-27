/* chd_alloc.c - libchdr's memory, from the Windows process heap (elsewhere
 * the C library's own allocator; chd_alloc.h).  The standard behaviours:
 * calloc zeroes (and refuses a count x size that overflows), realloc(NULL,
 * n) allocates, realloc(p, 0) frees and returns NULL, free(NULL) does
 * nothing. */
#ifdef _WIN32
#include <windows.h>

void *gt3_chd_malloc(size_t size)
{
    return HeapAlloc(GetProcessHeap(), 0, size);
}

void *gt3_chd_calloc(size_t count, size_t size)
{
    if (size != 0 && count > (size_t)-1 / size)
        return NULL;
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * size);
}

void *gt3_chd_realloc(void *block, size_t size)
{
    if (block == NULL)
        return HeapAlloc(GetProcessHeap(), 0, size);
    if (size == 0) {
        HeapFree(GetProcessHeap(), 0, block);
        return NULL;
    }
    return HeapReAlloc(GetProcessHeap(), 0, block, size);
}

void gt3_chd_free(void *block)
{
    if (block != NULL)
        HeapFree(GetProcessHeap(), 0, block);
}
#else
/* Off Windows the game's heap replaces malloc and the rest by their names
 * (the executable's own definitions win over the C library's): glibc's
 * allocator is reached under the names it also exports. */
#include <stddef.h>

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *block, size_t size);
extern void __libc_free(void *block);

void *gt3_chd_malloc(size_t size)
{
    return __libc_malloc(size);
}

void *gt3_chd_calloc(size_t count, size_t size)
{
    if (size != 0 && count > (size_t)-1 / size)
        return NULL;
    return __libc_calloc(count, size);
}

void *gt3_chd_realloc(void *block, size_t size)
{
    if (block == NULL)
        return __libc_malloc(size);
    if (size == 0) {
        __libc_free(block);
        return NULL;
    }
    return __libc_realloc(block, size);
}

void gt3_chd_free(void *block)
{
    if (block != NULL)
        __libc_free(block);
}
#endif
