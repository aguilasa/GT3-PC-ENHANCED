/* frame_pacing.h - the display side of the dynamic frame rate (no Windows
 * headers: the game code includes it; gpu.cpp implements it).
 *
 * The game ticks at 59.94 Hz (PC 2026-11-19: on a 59 .. 61 Hz display at its
 * own vblanks - gpu.cpp times them, scheduler.h set_display_clock); on a
 * display that refreshes faster, the race
 * draws frames between the ticks (game/app/pc_frame.h).  No vsync (the
 * default): the frames are presented at once, tearing allowed (a variable
 * refresh display follows them), one frame queued, Present's wait for the
 * GPU done outside the game CPU (gpu::present, sched::run_outside). */
#pragma once

namespace pacing {

/* The display refreshes faster than the game ticks (above 61 Hz). */
bool high_refresh();
/* The refresh rate in Hz. */
double refresh_hz();
/* Presents at the display's vblank (frames between ticks) or at once (the
 * game's own 59.94 Hz pacing). */
void set_vsync(bool on);
/* sched::now_seconds() when the last Present returned: when the display,
 * the GPU or a limiter outside the game (RTSS) let the frame go - the
 * frame clock of the dynamic frame rate (game/app/pc_frame.cpp). */
double last_present_time();
/* Seconds spent in Present so far (all frames): waiting for the GPU, the
 * display (vsync) or a limiter - what holds the frames back when the game's
 * own cap does not (pc_frame's statistics). */
double present_wait_total();
/* The size of the picture area: the back buffer (the window's client area,
 * or the fullscreen mode) - the race's dynamic aspect (game/app/pc_aspect.h). */
void output_size(int *w, int *h);

/* Multi-monitor (gpu.h "Screens"): the screens open (1: the game window's
 * alone), the canvas a race picture over all of them covers, screen i's x
 * and width in it; the span is on; the split screen asks for its second
 * monitor (each frame it wants it). */
int screen_count();
void canvas_size(int *w, int *h);
void canvas_rect(int i, int *x, int *w);
bool span_active();
void want_split_screen();

} // namespace pacing
