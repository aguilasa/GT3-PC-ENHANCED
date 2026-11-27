/* scheduler.h - the game threads of GT3 PC.
 *
 * The game was written for the EE kernel: one CPU, threads with priorities
 * 0..127 (lower runs first), a running thread keeps the CPU until it blocks
 * or a higher-priority thread becomes ready.  The code relies on that (the
 * render thread and the main thread share data without locks), so the port
 * keeps it: every game thread is a native thread, and exactly one of them
 * runs at a time, the one this scheduler picks.  Switches happen inside the
 * kernel calls (eekernel.h, implemented on this API by ee_kernel.cpp) and
 * at sched_preempt_point(), which long engine loops call.
 *
 * Interrupt handlers (vblank) run on the timer thread, concurrently with
 * the running game thread as real interrupts would, but never inside a
 * game thread's DI section.  A thread an interrupt makes ready preempts the
 * running thread at its next kernel call or preemption point.
 *
 * Native helper threads (window, audio, file streaming) are not game
 * threads: they must not call the blocking functions. */
#pragma once

#include <stdint.h>

namespace sched {

enum Status {               /* ThreadParam.status values */
    ST_RUN = 0x01, ST_READY = 0x02, ST_WAIT = 0x04, ST_SUSPEND = 0x08,
    ST_WAITSUSPEND = 0x0C, ST_DORMANT = 0x10
};
enum WaitType { WAIT_NONE = 0, WAIT_SLEEP = 1, WAIT_SEMA = 2, WAIT_DELAY = 3,
                WAIT_NATIVE = 4 /* PC: in a native call outside the CPU (run_outside) */ };

struct ThreadInfo {
    int status, initPriority, currentPriority, waitType, waitId, wakeupCount;
    void (*entry)(void *);
};
struct SemaInfo { int currentCount, maxCount, initCount, numWaitThreads; };

/* Starts the scheduler and the timer thread. */
void init();

/* Creates game thread 1 running entry(arg) at `priority' and returns once it
 * has been dispatched (the caller is not a game thread). */
void start_main_thread(void (*entry)(void *), void *arg, int priority, int stackSize);

/* --- threads (ids > 0; negative results are errors as on the PS2) ------ */
/* `dispatch' false is the EE's i-variant (iWakeupThread, iSignalSema...):
 * the state changes but the calling thread keeps running; a thread it made
 * ready runs at the caller's next switch point. */
int  create_thread(void (*entry)(void *), int priority, int stackSize);
int  delete_thread(int tid);
int  start_thread(int tid, void *arg);
void exit_thread(bool deleteIt);            /* does not return */
int  terminate_thread(int tid);
int  change_priority(int tid, int priority, bool dispatch = true);
int  rotate_ready_queue(int priority, bool dispatch = true);
int  release_wait(int tid, bool dispatch = true);
int  current_thread();                      /* 0 outside the game threads */
int  refer_thread(int tid, ThreadInfo *out);
int  sleep_thread();
int  wakeup_thread(int tid, bool dispatch = true);
int  cancel_wakeup(int tid);
int  suspend_thread(int tid, bool dispatch = true);
int  resume_thread(int tid, bool dispatch = true);
int  delay_thread(int usec);

/* --- semaphores ----------------------------------------------------------- */
int  create_sema(int initCount, int maxCount);
int  delete_sema(int sid);
int  signal_sema(int sid, bool dispatch = true);
int  wait_sema(int sid);
int  poll_sema(int sid);
int  refer_sema(int sid, SemaInfo *out);

/* --- interrupts ------------------------------------------------------------ */
enum { INTC_VBLANK_START = 2, INTC_VBLANK_END = 3 };
int  add_intc_handler(int cause, int (*handler)(int cause, void *arg, void *addr), void *arg,
                      int (*plain)(int cause), int first);
int  remove_intc_handler(int cause, int id);
int  enable_intc(int cause, bool on);
/* DI / EI of the calling game thread (no-ops elsewhere); di returns whether
 * interrupts were on. */
int  di();
void ei();
/* Alarms: callback(id, time, arg) after `hsyncs' horizontal lines (15734 Hz). */
int  set_alarm(uint16_t hsyncs, void (*cb)(int, uint16_t, void *), void *arg);
int  release_alarm(int id);

/* Lets a higher-priority thread that an interrupt made ready run now. */
void preempt_point();

/* The evidence of a softlock - every game thread's state, priority, wait
 * (the semaphore and its count) and entry, and the code addresses on its
 * stack (crash::log_stack) - to the log, headed `why'.  From a native thread
 * (the window's, as it closes). */
void log_threads(const char *why);

/* Runs fn(arg) - a native call that may block (the swap chain's Present
 * waiting for the display) - without the CPU: the other game threads run
 * meanwhile, as during a blocking kernel call; then the caller waits for
 * its turn.  fn must not touch what the other game threads use.  Outside a
 * game thread: just the call. */
void run_outside(void (*fn)(void *), void *arg);

/* Vblank count since start (59.94 Hz; PC 2026-11-19: or the display's,
 * set_display_clock). */
uint64_t vblank_count();
/* now_seconds() of the last vblank (its due time), and the period (the
 * current one: the display's while the vblanks follow it). */
double   vblank_time();
double   vblank_period();
/* PC (2026-11-19): the vblanks follow the display (gpu.cpp's watcher of the
 * game window's monitor): on, every vblank falls on the display's grid -
 * displayVblank (a display vblank, now_seconds()) + k x period - plus a
 * small phase offset (GT3PC_VBLANK_PHASE_MS, default 1 ms after it); off,
 * the PS2's 59.94 Hz from the last one.  User report (2026-11-19): with
 * vsync at 60 Hz on a 60 Hz monitor every setting had a saw-tooth or
 * spikes every several seconds (RTSS's scanline sync too): the game's
 * 59.94 Hz vblanks drifted through the display's, a frame missed its
 * vblank each time they lined up wrong and the next caught up two ticks.
 * The switch moves the next vblank by up to half a period, once. */
void     set_display_clock(bool on, double period, double displayVblank);
/* PC (2026-11-19): the vblanks' offset after the display's (seconds, 0 ..
 * the period): gpu.cpp puts them half a period from where Present returns
 * (the frames paced by it: game/app/pc_frame.cpp), so the ticks counted at
 * those returns are one a frame, whatever their jitter.
 * GT3PC_VBLANK_PHASE_MS fixes it instead. */
void     set_vblank_phase(double seconds);
/* PC (2026-11-19): the vblanks follow the display's (set_display_clock on). */
bool     display_clock_on();
/* The vblanks due by time t (now_seconds()): the count of the last one at
 * or before t and its time (the last 31 are kept; an older t gives the
 * oldest of them). */
uint64_t vblanks_at(double t, double *time);
/* Seconds since start, high resolution. */
double   now_seconds();
/* A game thread waits until now_seconds() reaches t: DelayThread's sleep to
 * within 0.8 ms (the CPU given up), then the rest spun (the CPU kept): to a
 * few microseconds (the frame time cap, game/app/pc_frame.cpp). */
void     wait_until(double t);

} // namespace sched
