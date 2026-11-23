/* bgm_side.cpp - audio_test's side of the decompiled code: compiled with the
 * decomp's options (the prelude) as engine/pdi/T521_pdibgm.cpp is, it gives
 * T521 the few kernel calls it makes and hands the native test plain
 * functions over pdibgm_*. */
#include "eekernel.h"
#include "engine/media/G002_mpeg_frameq.h"
#include "engine/pdi/T521_pdibgm.h"

#include <stdarg.h>

/* ---- what T521 needs of the game's kernel and C library ----------------- */

void EI(void) {}
int intr_suspend(void) { return 1; }

extern "C" int pc_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    return n;
}

/* ---- for audio_test.cpp ---------------------------------------------------- */

void bgm_side_start(int sectors, int loop, const char *hostName)
{
    pdibgm_set_volume(1.0f);
    pdibgm_start(0, sectors, loop, 0x40000, hostName);
}

void bgm_side_stop(void)
{
    pdibgm_stop();
}

float bgm_side_status(int *ended)
{
    int pos = 0, id = 0;
    pdibgm_update();
    return pdibgm_get_status(&pos, &id, ended);
}
