/* chd_alloc.h - PC (2026-11-23): the memory of libchdr and the decoders it
 * carries (cmake/chd.cmake puts this header in front of their sources with
 * /FI), from the Windows process heap (chd_alloc.c).
 *
 * In gt3pc.exe the C library's malloc family is the game's own heap (src/
 * engine/pdistd/T532_heap.cpp), kept as the PS2's: its calloc does not zero
 * a block handed out before, and its realloc copies by the old block's size.
 * libchdr, zstd and dr_flac count on the standard ones (a zeroed chd_file, a
 * zeroed decoder state), so the library takes none of the four from the C
 * library: here they are other functions, in the launcher too.  Nothing the
 * library allocates is freed outside it, or the other way round. */
#ifndef GT3_CHD_ALLOC_H
#define GT3_CHD_ALLOC_H

/* (the C library's own declarations first: seen once, under their names) */
#include <stddef.h>
#include <stdlib.h>
#include <malloc.h>

#ifdef __cplusplus
extern "C" {
#endif
void *gt3_chd_malloc(size_t size);
void *gt3_chd_calloc(size_t count, size_t size);
void *gt3_chd_realloc(void *block, size_t size);
void gt3_chd_free(void *block);
#ifdef __cplusplus
}
#endif

#define malloc gt3_chd_malloc
#define calloc gt3_chd_calloc
#define realloc gt3_chd_realloc
#define free gt3_chd_free

#endif /* GT3_CHD_ALLOC_H */
