/* dr_mp3.c - the implementation of dr_mp3.h (single-header MP3 decoder,
 * README.md): one translation unit of the platform layer off Windows, which
 * decodes the .mp3 BGM (engine/pdi/T521_pdibgm.cpp).  The decoder reads
 * through callbacks only (no stdio). */
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
