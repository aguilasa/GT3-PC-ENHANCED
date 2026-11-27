/* pc_frame.cpp - the dynamic frame rate of GT3 PC (pc_frame.h).  No
 * original: new PC code. */
#include "game/app/pc_frame.h"
#include "game/app/pc_aspect.h"                 /* pc_aspect_frame_begin */
#include "game/app/pc_debugcam.h"               /* pc_debugcam_frame, pc_debugcam_step_frames */
#include "engine/pgl/G001_flare.h"              /* pgl_get_matrix / pgl_load_matrix, gfx_vsync */
#include "eekernel.h"                           /* DelayThread */
#include "libc.h"                               /* printf, sqrtf */
#include "platform/frame_monitor.h"
#include "platform/frame_pacing.h"
#include "platform/osd.h"                       /* the replay speed's message */
#include "platform/scheduler.h"
#include "platform/settings.h"                  /* the frame cap */
#include "game/app/pc_sound.h"                  /* pc_sound_tick */
#include "game/app/pc_units.h"                  /* pc_units_sync */
#include "game/app/pc_ffb.h"                    /* PC (2026-11-20): pc_ffb_sync */
#include "platform/frame_step.h"                /* the tick step (F5 / F6) */
#include "platform/ui_text.h"                   /* PC (2026-11-16): the interface's languages */

int g_pcTickFrame = 1;
f32 g_pcViewLag = 0.0f;
static f32 s_frameLag;                          /* the frame's (pc_frame_lag) */

/* The race asked for frames between the ticks during the last tick. */
static int s_request;
/* ... and the display refreshes faster than the tick or GT3PC_FRAME_RATE is
 * set: the loop runs free - frames at their own rate, before each the
 * ticks due by its clock (several when the frames are slower than the
 * ticks), drawn interpolated.  Else one frame per vblank, the original's
 * loop (the ticks missed while a frame took longer caught up too). */
static int s_interpolating;
/* The last tick run: its vblank count and time (sched::now_seconds). */
static u64 s_lastTick;
static double s_tickTime;
/* The frame clock: the time the frame being drawn shows (pc_frame_ticks),
 * and when pc_frame_ticks last ran. */
static double s_frameClock;
static double s_lastTicksCall;
/* Ticks so far (pc_view_interpolate tells a new tick by it). */
static u32 s_tickNo;
/* The vblank times of the ticks pc_frame_ticks gave (the first's; each next
 * one s_batchStep later: a vblank, but in a replay at another speed) and the
 * tick running: its own (the sound's block time, game/app/pc_sound.h). */
static double s_batchFirst;
static double s_batchStep;
static int s_batchIdx;
static double s_curTickTime;
/* GT3PC_FRAME_RATE's pacing: the next frame's time. */
static double s_nextFrame;
/* The ticks one frame catches up at most (7.5 frames a second): a longer
 * stall (a load) runs one, the game waits instead of racing ahead. */
enum { PC_MAX_TICKS = 8 };
/* A replay's speeds (L2 a step down, R2 a step up; pc_frame.h), and the ticks
 * a frame runs at most at them: frames too slow for a speed lose the rest,
 * not the frame rate. */
static const double kReplaySpeeds[] = {0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
static const char *const kReplaySpeedNames[] = {"1/8", "1/4", "1/2", "1X", "2X", "4X", "8X"};
enum { PC_REPLAY_SPEEDS = 7, PC_REPLAY_NORMAL = 3, PC_REPLAY_MAX_TICKS = 16 };
static int s_replayStep = PC_REPLAY_NORMAL;
/* The last tick ran a replay on screen (pc_replay_speed_request). */
static int s_replayRun;
/* The buttons held at the last pad read, by player. */
static int s_replayHeld[2][2];
/* The ticks of this frame ran at the replay's speed: s_speedFrac is how far
 * past the last tick the frame clock lies, in ticks, counted up to
 * s_speedClock. */
static int s_speedOn;
static double s_speedFrac;
static double s_speedClock;
/* Statistics, logged every 10 s while the loop runs free or ticks were
 * caught up / dropped. */
static double s_statStart;
static u32 s_statFrames, s_statTicks, s_statCaught, s_statDropped;
static u32 s_statNoTick;                        /* one frame per vblank: frames without a tick (none expected) */
static double s_statWait;                       /* pacing::present_wait_total() at the period's start */
/* The cap last logged (pc_frame_tick_end). */
static double s_loggedCap = -1.0;
/* The frame's iteration began (pc_frame_ticks returned; 0: not yet): the
 * frame monitor's UPDATE runs to pc_frame_render_begin. */
static double s_iterStart;

/* GT3PC_FRAME_RATE=<Hz> (a test switch): frames at that rate (10 .. 1000),
 * over the frame cap setting. */
static double forced_rate(void)
{
    static double rate = -1.0;
    if (rate < 0.0) {
        const char *e = getenv("GT3PC_FRAME_RATE");
        rate = e != NULL ? atof(e) : 0.0;
        if (rate < 10.0)
            rate = 0.0;
        else {
            if (rate > 1000.0)
                rate = 1000.0;
            printf("frame: GT3PC_FRAME_RATE: %.0f frames a second (over the frame cap setting)\n", rate);
        }
    }
    return rate;
}

/* The frame time cap in seconds, 0 none: [Graphics] FrameCap (the settings
 * menu, docs/ROADMAP.md item 7), or GT3PC_FRAME_RATE's. */
static double cap_period(void)
{
    const double env = forced_rate();
    if (env > 0.0)
        return 1.0 / env;
    const double ms = settings::frame_cap_ms();
    return ms > 0.0 ? ms * 0.001 : 0.0;
}

void pc_frame_request_interpolation(void)
{
    s_request = 1;
}

void pc_replay_speed_request(void)
{
    s_replayRun = 1;
}

void pc_replay_speed_reset(void)
{
    s_replayStep = PC_REPLAY_NORMAL;
}

/* The ticks' rate against the vblanks: the normal rate unless a replay ran in
 * the last tick. */
static double replay_speed(void)
{
    return s_replayRun != 0 ? kReplaySpeeds[s_replayStep] : 1.0;
}

void pc_replay_speed_buttons(int port, int slower, int faster)
{
    int *held = s_replayHeld[port & 1];
    const int down = slower != 0 && held[0] == 0, up = faster != 0 && held[1] == 0;

    held[0] = slower != 0;
    held[1] = faster != 0;
    if (s_replayRun == 0 || (down == 0 && up == 0))
        return;
    int step = s_replayStep + up - down;
    if (step < 0)
        step = 0;
    if (step >= PC_REPLAY_SPEEDS)
        step = PC_REPLAY_SPEEDS - 1;
    if (step != s_replayStep) {
        s_replayStep = step;
        printf("frame: replay speed %.3f\n", kReplaySpeeds[step]);
    }
    /* (at the end of the range too: the press is answered) */
    osd::show(trf("REPLAY SPEED %s", kReplaySpeedNames[step]).c_str());   /* (PC 2026-11-16: translated) */
}

int pc_frame_interpolating(void)
{
    return s_interpolating;
}

void pc_frame_tick_begin(void)
{
    pc_units_sync();                            /* a units setting changed (game/app/pc_units.h) */
    pc_ffb_sync();                              /* PC (2026-11-20): GT3's force feedback option (game/app/pc_ffb.h) */
    s_request = 0;
    s_replayRun = 0;
    s_tickNo++;
    s_curTickTime = s_batchFirst + (double)s_batchIdx++ * s_batchStep;
}

void pc_frame_tick_end(void)
{
    /* the sound: this tick's flush, at its vblank's time (pc_sound.h) */
    pc_sound_tick(s_curTickTime);
    /* the other game threads' vsync (the menus' loops read the pads this
     * tick updated: once each, however the ticks bunch up) */
    gfx_tick_vsync();
    /* frames between the ticks: on a display faster than the tick, with a
     * frame cap (its rate whatever the display: below 60 a second too, the
     * ticks caught up), or with neither cap nor vsync (PC: uncapped on a 60 Hz
     * display too - user, 2026-10-20: the cap OFF stayed at one frame per
     * vblank there), or in a replay's slow motion on any display (one frame
     * a tick would be 7.5 frames a second at 1/8) */
    const double cap = cap_period();
    int on = s_request != 0 && (cap > 0.0 || pacing::high_refresh() || settings::vsync() == 0 || replay_speed() < 1.0);

    if (on == s_interpolating && (!on || cap == s_loggedCap))
        return;
    s_interpolating = on;
    s_loggedCap = cap;
    /* the frames as fast as they are drawn, or at the cap, or as vsync (the
     * settings, gpu.cpp) or a limiter outside the game (RTSS) lets them go */
    if (on && cap > 0.0)
        printf("frame: free-running, capped at %.3f ms (%.2f frames a second), interpolated\n", cap * 1000.0,
               1.0 / cap);
    else if (on)
        printf("frame: free-running, uncapped (display %.0f Hz, vsync %s), interpolated\n", pacing::refresh_hz(),
               settings::vsync() > 0 ? "on" : "off");
    else
        printf("frame: one frame per vblank\n");
}

void pc_frame_render_begin(int tick)
{
    pc_aspect_frame_begin();                    /* the 4:3 picture unless a race view draws */
    pc_debugcam_frame();                        /* the debug camera's controls, by the real time */
    g_pcTickFrame = tick;
    frame_monitor::note_frame(tick, s_interpolating);
    if (s_iterStart > 0.0)                      /* the ticks' time (the pads, the updates, the sound) */
        frame_monitor::add(frame_monitor::PART_UPDATE, (sched::now_seconds() - s_iterStart) * 1000.0);
    s_iterStart = 0.0;
    if (!s_interpolating) {
        g_pcViewLag = s_frameLag = 0.0f;
        return;
    }
    /* the shown time: the frame clock one tick behind, so that it lies
     * between the last two ticks' states (whatever the frame rate: the
     * ticks due by the clock have run) */
    double f = s_speedOn != 0 ? s_speedFrac : (s_frameClock - s_tickTime) / sched::vblank_period();
    if (f < 0.0)
        f = 0.0;
    if (f > 1.0)
        f = 1.0;
    g_pcViewLag = s_frameLag = (f32)(1.0 - f);
}

f32 pc_frame_lag(void)
{
    return s_frameLag;
}

/* The frame's end work (pc_frame_at_end). */
struct PcAtEnd {
    void (*fn)(void *arg);
    void *arg;
};
static PcAtEnd s_atEnd[4];
static int s_atEndCount;

void pc_frame_at_end(void (*fn)(void *arg), void *arg)
{
    for (int i = 0; i < s_atEndCount; i++) {
        if (s_atEnd[i].fn == fn && s_atEnd[i].arg == arg)
            return;
    }
    if (s_atEndCount < (int)(sizeof(s_atEnd) / sizeof(s_atEnd[0]))) {
        s_atEnd[s_atEndCount].fn = fn;
        s_atEnd[s_atEndCount].arg = arg;
        s_atEndCount++;
    }
}

void pc_frame_render_end(void)
{
    for (int i = 0; i < s_atEndCount; i++)
        s_atEnd[i].fn(s_atEnd[i].arg);
    s_atEndCount = 0;
    g_pcViewLag = s_frameLag = 0.0f;
    g_pcTickFrame = 1;
}

/* PC (2026-11-19): one frame per vblank paced by Present - vsync on and the
 * game's vblanks on the display's (platform/scheduler.h set_display_clock):
 * no wait for the game's own vblank, Present's for the display's alone, and
 * the ticks those of the grid at its return (pc_frame_ticks).  User report:
 * "Vertical sync ... at 60hz in-game with a 60hz monitor, all settings have
 * saw-tooth, or spikes ... every several seconds": two waits a frame - the
 * game's 59.94 Hz vblank, then the display's in Present - whose phases
 * drifted round each other: Present's wait grew and shrank (the log's 0.5 ..
 * 9.6 ms a frame) and a frame missed its vblank each time round, the next
 * catching up two ticks.  Locked to the display, two waits still left the
 * frame what Present's wait did not take (the harness: 3.7 ms of 16.7).
 * (A display at 2 .. 4 times the game's rate - the game's vblanks on every
 * 2nd .. 4th of its - not: a frame per display vblank would be 2 .. 4 a tick;
 * those wait for the game's vblank, or draw the frames between the ticks.) */
static int present_paced(void)
{
    return !s_interpolating && settings::vsync() > 0 && !pacing::high_refresh() && sched::display_clock_on();
}
/* ... and a frame of it that found no tick due: the next waits for the
 * game's vblank after all (a Present that does not wait - vsync forced off
 * outside the game - spinning frames without ticks) */
static int s_pacedIdle;

/* The wait itself (pc_frame_wait, timed as the frame monitor's PACE). */
static void frame_wait(void)
{
    /* One frame per vblank: the original's wait (a vblank since the last).
     * Between the ticks: the frames go as fast as they are drawn (Present
     * waits for the GPU or the display, outside the game CPU, or for a
     * limiter hooked into it).  PC (2026-11-19): with vsync on the display's
     * vblanks, Present's wait alone (present_paced). */
    if (!s_interpolating && (!present_paced() || s_pacedIdle))
        gfx_vsync(-1);
    /* The frame time cap: the frames at least that far apart, by time (one
     * per vblank: only a cap longer than the tick waits more; the ticks due
     * by the clock are caught up) */
    const double period = cap_period();
    if (period <= 0.0 || (!s_interpolating && period <= sched::vblank_period() * 1.001)) {
        s_nextFrame = 0.0;
        return;
    }
    const double now = sched::now_seconds();
    if (s_nextFrame < now - period || s_nextFrame > now + period)
        s_nextFrame = now;                      /* late by a frame or more (or the cap changed): from now */
    sched::wait_until(s_nextFrame);             /* (to a few microseconds) */
    s_nextFrame += period;
}

void pc_frame_wait(void)
{
    const double t0 = sched::now_seconds();
    frame_wait();
    frame_monitor::add(frame_monitor::PART_PACE, (sched::now_seconds() - t0) * 1000.0);
}

/* The tick step's pause (platform/frame_step.h): no tick and no drawing -
 * the picture shown presented again about every vblank (its overlays alive),
 * the game's other threads running - until a step, F6 or the debug camera
 * coming on (it needs frames: pc_debugcam_step_frames).  1: a step. */
static int step_hold(void)
{
    while (frame_step::paused() && !pc_debugcam_step_frames()) {
        if (frame_step::take_step())
            return 1;
        gfx_present_again();
        DelayThread(16000);
    }
    return 0;
}

/* The whole ticks a replay at `speed' has due by the clock, the fraction left
 * in s_speedFrac.  From the normal speed it goes on from the tick's fraction
 * as it is: at the new rate that part of a tick moved the shown time back,
 * or on by several ticks.  More than a frame's most: the rest is lost. */
static int replay_ticks(double clock, double speed)
{
    const double period = sched::vblank_period();

    if (s_speedOn == 0) {
        s_speedOn = 1;
        s_speedFrac = (clock - s_tickTime) / period;
        if (s_speedFrac > 1.0)
            s_speedFrac = 1.0;                  /* (a stall: not caught up) */
        s_speedClock = clock;
    }
    s_speedFrac += (clock - s_speedClock) * speed / period;
    s_speedClock = clock;
    if (s_speedFrac < 0.0)
        s_speedFrac = 0.0;
    const int whole = (int)s_speedFrac;
    s_speedFrac -= (double)whole;
    return whole > PC_REPLAY_MAX_TICKS ? PC_REPLAY_MAX_TICKS : whole;
}

int pc_frame_ticks(void)
{
    if (frame_step::paused()) {
        double t = 0.0;
        s_speedOn = 0;                          /* (the tick step's own ticks, at the last tick) */
        /* (the debug camera, on now or earlier in this pause: frames drawn as
         * a race's between ticks, at one a vblank without interpolation;
         * game/app/pc_debugcam.h) */
        int step;
        if (s_interpolating || pc_debugcam_step_frames())
            step = frame_step::take_step() ? 1 : 0;
        else
            step = step_hold();
        if (!step && frame_step::paused()) {
            /* A race drawing frames between ticks: the frames go on at the
             * last tick (lag 0), as fast as before (or the cap), no tick.
             * Held instead, the GPU and the CPU fell to their idle clocks -
             * the deeper the longer - and the first frames after F6 ran
             * slow while they rose again (user report).  The vblanks go by
             * uncounted. */
            const double now = sched::now_seconds();
            s_lastTick = sched::vblanks_at(now, &t);
            s_frameClock = s_tickTime + sched::vblank_period();
            s_lastTicksCall = now;
            return 0;
        }
        const double now = sched::now_seconds();
        const u64 due = sched::vblanks_at(now, &t);
        if (step) {
            /* one tick, at the last vblank, its frame drawn at the tick (no
             * interpolation: the clock a tick past it) */
            s_lastTick = due;
            s_tickTime = t;
            s_frameClock = t + sched::vblank_period();
            s_lastTicksCall = now;
            s_batchFirst = t;
            s_batchIdx = 0;
            if (s_interpolating)
                gfx_vsync_mark();
            return 1;
        }
        /* running again: on from now (the paused time is not caught up) */
        s_lastTick = due > 0 ? due - 1 : 0;
    }
    const double now = sched::now_seconds();
    double clock = now;
    if (s_interpolating || present_paced()) {   /* (PC 2026-11-19: paced by Present: its return too) */
        /* The frame clock: when the last Present returned (in the frame
         * just ended).  The frame drawn next is presented the same way one
         * frame later (T002 flips to it after drawing the following one),
         * so whatever paces the frames - GT3PC_FRAME_RATE's wait, a limiter
         * stalling Present (RTSS), the GPU, a variable refresh display - the
         * shown time keeps the same distance to the display.  (Sampled when
         * the frame's drawing began, it moved with the update's time on the
         * tick frames and the execution's: frames off by a few ms against
         * the display, choppy at 90 a second.) */
        const double q = pacing::last_present_time();
        if (q > s_lastTicksCall && q <= now)
            clock = q;
    }
    s_lastTicksCall = now;

    /* the ticks due by the clock (the vblanks at or before it); more than
     * PC_MAX_TICKS is a stall (a load, the first frame), not caught up: one */
    double t = 0.0;
    const u64 due = sched::vblanks_at(clock, &t);
    u64 n = due > s_lastTick ? due - s_lastTick : 0;
    const double speed = replay_speed();
    if (speed != 1.0) {
        /* a replay at another speed: its own clock; the vblanks go on being
         * counted, for the normal speed to go on from */
        n = (u64)replay_ticks(clock, speed);
        s_batchStep = sched::vblank_period() / speed;
        t = clock - s_speedFrac * s_batchStep;  /* (its last tick's time) */
        s_lastTick = due;
        s_tickTime = t;
    } else {
        if (s_speedOn != 0) {
            /* back at the normal speed: its fraction of a tick as it is */
            s_speedOn = 0;
            s_tickTime = clock - s_speedFrac * sched::vblank_period();
        }
        s_batchStep = sched::vblank_period();
        if (n > PC_MAX_TICKS) {
            s_statDropped += (u32)(n - 1);
            n = 1;
        }
        if (due > s_lastTick) {
            s_lastTick = due;
            s_tickTime = t;
        }
    }
    /* the ticks' own vblank times: the last due at t, the others before it */
    s_batchFirst = t - (double)(n > 0 ? n - 1 : 0) * s_batchStep;
    s_batchIdx = 0;
    s_frameClock = clock;
    /* the driver's vsync state, as gfx_vsync(-1) kept it - in one frame per
     * vblank too: a vblank whose tick was caught up here (the frame's present
     * and execution ran past it) counts as waited for.  (Marked only while
     * interpolating, as before, the next gfx_vsync(-1) returned at once for
     * that vblank and the frame after ran no tick: frames between ticks in
     * the menus, T002.) */
    if (n > 0)
        gfx_vsync_mark();
    if (n == 0 && !s_interpolating)
        s_statNoTick++;
    s_pacedIdle = n == 0 && present_paced();    /* PC (2026-11-19) */

    if (s_statStart == 0.0) {
        s_statStart = now;
        s_statWait = pacing::present_wait_total();
    }
    s_statFrames++;
    s_statTicks += (u32)n;
    if (n > 1)
        s_statCaught += (u32)(n - 1);
    s_iterStart = sched::now_seconds();         /* (the monitor's UPDATE from here) */
    if (now - s_statStart >= 10.0) {
        /* (PC: the rate, what paces it, and Present's wait a frame: the GPU,
         * the display or a limiter outside the game when the cap does not) */
        const double wait = pacing::present_wait_total();
        if (s_interpolating || present_paced() || s_statCaught != 0 || s_statDropped != 0 || s_statNoTick != 0) {
            char mode[48];
            char noTick[40] = "";
            if (s_statNoTick != 0)
                snprintf(noTick, sizeof(noTick), ", %u frame%s without one", s_statNoTick,
                         s_statNoTick == 1 ? "" : "s");
            const double cap = cap_period();
            if (present_paced())
                snprintf(mode, sizeof(mode), "one per display vblank, paced by Present");    /* PC (2026-11-19) */
            else if (!s_interpolating)
                snprintf(mode, sizeof(mode), "one per vblank");
            else if (cap > 0.0)
                snprintf(mode, sizeof(mode), "capped at %.1f", 1.0 / cap);
            else
                snprintf(mode, sizeof(mode), "uncapped");
            printf("frame: %u frames (%.1f a second, %s), %u ticks (%u caught up, %u dropped%s) in %.1f s; Present"
                   " waited %.2f ms a frame\n", s_statFrames, s_statFrames / (now - s_statStart), mode, s_statTicks,
                   s_statCaught, s_statDropped, noTick, now - s_statStart,
                   s_statFrames ? (wait - s_statWait) * 1000.0 / s_statFrames : 0.0);
        }
        s_statStart = now;
        s_statWait = wait;
        s_statFrames = s_statTicks = s_statCaught = s_statDropped = s_statNoTick = 0;
    }
    return (int)n;
}

/* ======================================================================== */
/* The views                                                                */
/* ======================================================================== */

/* A race view's matrices at the ticks of its last two draws (row vectors,
 * translation in row 3: pgl's layout): one tick apart while the frames
 * are faster than the ticks, more when they are slower (the ticks caught
 * up: no draw in between). */
struct PcView {
    const void *key;
    u32 shot;                   /* cur's camera and mount (pc_view_interpolate) */
    u32 tick;                   /* the tick of cur */
    u32 gap;                    /* ticks from prev to cur */
    int have;                   /* 0 none, 1 cur, 2 prev and cur */
    f32 prevView[16], prevProj[16];
    f32 curView[16], curProj[16];
};
/* prev older than this many ticks is not blended (a stall) */
enum { PC_VIEW_MAX_GAP = PC_MAX_TICKS };
/* (a split screen: two views and their rear-view mirrors) */
enum { PC_VIEW_SLOTS = 8 };
static PcView s_views[PC_VIEW_SLOTS];

static PcView *view_slot(const void *key)
{
    PcView *oldest = &s_views[0];
    for (int i = 0; i < PC_VIEW_SLOTS; i++) {
        if (s_views[i].key == key)
            return &s_views[i];
        if (s_views[i].tick < oldest->tick)
            oldest = &s_views[i];
    }
    oldest->key = key;
    oldest->have = 0;
    return oldest;
}

static f32 dot3(const f32 *a, const f32 *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* The camera's world position of a rigid view: c A + t = 0, c = -t A^-1. */
static void view_eye(const f32 *m, f32 *c)
{
    const f32 *r0 = m, *r1 = m + 4, *r2 = m + 8, *t = m + 12;
    f32 a = r0[0], b = r0[1], cc = r0[2], d = r1[0], e = r1[1], f = r1[2], g = r2[0], h = r2[1], k = r2[2];
    f32 det = a * (e * k - f * h) - b * (d * k - f * g) + cc * (d * h - e * g);
    if (det == 0.0f)
        det = 1.0f;
    /* inverse rows */
    f32 i00 = (e * k - f * h) / det, i01 = (cc * h - b * k) / det, i02 = (b * f - cc * e) / det;
    f32 i10 = (f * g - d * k) / det, i11 = (a * k - cc * g) / det, i12 = (cc * d - a * f) / det;
    f32 i20 = (d * h - e * g) / det, i21 = (b * g - a * h) / det, i22 = (a * e - b * d) / det;
    c[0] = -(t[0] * i00 + t[1] * i10 + t[2] * i20);
    c[1] = -(t[0] * i01 + t[1] * i11 + t[2] * i21);
    c[2] = -(t[0] * i02 + t[1] * i12 + t[2] * i22);
}

static f32 det3(const f32 *m)
{
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
           m[2] * (m[4] * m[9] - m[5] * m[8]);
}

/* Quaternion of the orthonormalized rotation rows of m (x, y, z, w), and
 * back, in one convention (a round trip gives the rows again). */
static void rows_to_quat(const f32 *m, f32 *q)
{
    f32 r[3][3];
    for (int i = 0; i < 3; i++) {
        f32 len = sqrtf(dot3(m + 4 * i, m + 4 * i));
        for (int j = 0; j < 3; j++)
            r[i][j] = len > 0.0f ? m[4 * i + j] / len : 0.0f;
    }
    f32 trace = r[0][0] + r[1][1] + r[2][2];   /* (PC 2026-11-16: `tr' before - ui_text.h's function) */
    if (trace > 0.0f) {
        f32 s = sqrtf(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (r[2][1] - r[1][2]) / s;
        q[1] = (r[0][2] - r[2][0]) / s;
        q[2] = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        f32 s = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q[3] = (r[2][1] - r[1][2]) / s;
        q[0] = 0.25f * s;
        q[1] = (r[0][1] + r[1][0]) / s;
        q[2] = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        f32 s = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q[3] = (r[0][2] - r[2][0]) / s;
        q[0] = (r[0][1] + r[1][0]) / s;
        q[1] = 0.25f * s;
        q[2] = (r[1][2] + r[2][1]) / s;
    } else {
        f32 s = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q[3] = (r[1][0] - r[0][1]) / s;
        q[0] = (r[0][2] + r[2][0]) / s;
        q[1] = (r[1][2] + r[2][1]) / s;
        q[2] = 0.25f * s;
    }
}

static void quat_to_rows(const f32 *q, f32 r[3][3])
{
    f32 x = q[0], y = q[1], z = q[2], w = q[3];
    r[0][0] = 1.0f - 2.0f * (y * y + z * z);
    r[0][1] = 2.0f * (x * y - z * w);
    r[0][2] = 2.0f * (x * z + y * w);
    r[1][0] = 2.0f * (x * y + z * w);
    r[1][1] = 1.0f - 2.0f * (x * x + z * z);
    r[1][2] = 2.0f * (y * z - x * w);
    r[2][0] = 2.0f * (x * z - y * w);
    r[2][1] = 2.0f * (y * z + x * w);
    r[2][2] = 1.0f - 2.0f * (x * x + y * y);
}

/* A camera cut between the two ticks (a change of camera): not blended.
 * More than 20 m of eye travel, or the view axis turned more than about
 * 45 degrees, or the projection's x scale changed by a third. */
static int view_cut(const f32 *v0, const f32 *v1, const f32 *p0, const f32 *p1)
{
    f32 c0[3], c1[3];
    view_eye(v0, c0);
    view_eye(v1, c1);
    f32 dx = c1[0] - c0[0], dy = c1[1] - c0[1], dz = c1[2] - c0[2];
    if (dx * dx + dy * dy + dz * dz > 400.0f)
        return 1;
    /* the view's z axis in the world: column 2 of the rotation rows */
    f32 z0[3] = {v0[2], v0[6], v0[10]}, z1[3] = {v1[2], v1[6], v1[10]};
    f32 l = sqrtf(dot3(z0, z0) * dot3(z1, z1));
    if (l <= 0.0f || dot3(z0, z1) < 0.7f * l)
        return 1;
    if (p0[0] == 0.0f || (p1[0] - p0[0]) * (p1[0] - p0[0]) > (p0[0] * p0[0]) / 9.0f)
        return 1;
    return 0;
}

/* m = the blend of a (at 0) and b (at 1): the eye moved linearly, the
 * rotation along the shorter arc (normalized lerp of quaternions), the
 * rows' lengths linearly.  Two mirrored views (the rear-view mirror's, z
 * flipped) are blended unmirrored (column 2 negated: the same eye, a proper
 * rotation) and mirrored again; one mirrored, or a degenerate view, element
 * by element. */
static void view_blend(const f32 *a, const f32 *b, f32 s, f32 *m)
{
    const f32 da = det3(a), db = det3(b);
    if (da < 0.0f && db < 0.0f) {
        f32 ua[16], ub[16];
        for (int i = 0; i < 16; i++) {
            ua[i] = (i & 3) == 2 ? -a[i] : a[i];
            ub[i] = (i & 3) == 2 ? -b[i] : b[i];
        }
        view_blend(ua, ub, s, m);
        for (int i = 2; i < 16; i += 4)
            m[i] = -m[i];
        return;
    }
    for (int i = 0; i < 16; i++)
        m[i] = a[i] + (b[i] - a[i]) * s;
    if (da <= 0.0f || db <= 0.0f)
        return;
    f32 qa[4], qb[4], q[4];
    rows_to_quat(a, qa);
    rows_to_quat(b, qb);
    f32 d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    f32 sign = d < 0.0f ? -1.0f : 1.0f;
    for (int i = 0; i < 4; i++)
        q[i] = qa[i] + (qb[i] * sign - qa[i]) * s;
    f32 len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len <= 0.0f)
        return;
    for (int i = 0; i < 4; i++)
        q[i] /= len;
    f32 r[3][3];
    quat_to_rows(q, r);
    f32 ca[3], cb[3], c[3];
    view_eye(a, ca);
    view_eye(b, cb);
    for (int i = 0; i < 3; i++) {
        c[i] = ca[i] + (cb[i] - ca[i]) * s;
        f32 la = sqrtf(dot3(a + 4 * i, a + 4 * i)), lb = sqrtf(dot3(b + 4 * i, b + 4 * i));
        f32 l = la + (lb - la) * s;
        for (int j = 0; j < 3; j++)
            m[4 * i + j] = r[i][j] * l;
    }
    /* t = -c A */
    for (int k = 0; k < 3; k++)
        m[12 + k] = -(c[0] * m[k] + c[1] * m[4 + k] + c[2] * m[8 + k]);
}

void pc_view_interpolate(const void *key, u32 shot)
{
    f32 view[16], proj[16];

    pgl_get_matrix(view);                       /* MODELVIEW: the view just applied */
    pgl_matrix_mode(PGL_MTX_PROJECTION);
    pgl_get_matrix(proj);
    PcView *v = view_slot(key);
    if (v->have == 0 || v->tick != s_tickNo) {
        /* the first draw after a tick: the last one becomes the previous
         * (not across a change of camera or mount: the view button's
         * bumper / chase switch moves the eye a few metres, which the
         * distances below take for motion) */
        const u32 gap = s_tickNo - v->tick;
        if (v->have != 0 && gap >= 1 && gap <= PC_VIEW_MAX_GAP && v->shot == shot &&
            !view_cut(v->curView, view, v->curProj, proj)) {
            memcpy(v->prevView, v->curView, sizeof(view));
            memcpy(v->prevProj, v->curProj, sizeof(proj));
            v->have = 2;
            v->gap = gap;
        } else {
            v->have = 1;
        }
        memcpy(v->curView, view, sizeof(view));
        memcpy(v->curProj, proj, sizeof(proj));
        v->tick = s_tickNo;
        v->shot = shot;
    }
    if (g_pcViewLag > 0.0f && v->have == 2) {
        /* the shown time lag ticks before the last; prev is gap ticks
         * before it (gap > 1: the camera taken as moving evenly between) */
        f32 s = 1.0f - g_pcViewLag / (f32)v->gap;   /* 0: prev's tick, 1: the last */
        f32 m[16];
        for (int i = 0; i < 16; i++)
            m[i] = v->prevProj[i] + (v->curProj[i] - v->prevProj[i]) * s;
        pgl_load_matrix(m);
        pgl_matrix_mode(PGL_MTX_MODELVIEW);
        view_blend(v->prevView, v->curView, s, m);
        pgl_load_matrix(m);
        return;
    }
    /* not blended (a cut, the first ticks of a view): the view is the
     * tick's, and so is the rest of it - the cars and the HUD at the shown
     * time under a camera at the tick moved against it until the next tick
     * (the bumper / chase switch in motion showed an in-between frame: user
     * report); the next view starts from the frame's lag again */
    g_pcViewLag = 0.0f;
    pgl_matrix_mode(PGL_MTX_MODELVIEW);
}

/* ======================================================================== */
/* The animations stepped once a tick                                       */
/* ======================================================================== */

struct PcAnim {
    const void *key;
    u32 tick;                   /* the tick of the last step */
    f32 dt;                     /* that tick's steps */
};
static PcAnim s_anims[64];      /* a course and its cars */

void pc_anim_stepped(const void *key, f32 dt)
{
    PcAnim *slot = &s_anims[0];
    for (int i = 0; i < (int)(sizeof(s_anims) / sizeof(s_anims[0])); i++) {
        PcAnim *a = &s_anims[i];
        if (a->key == key) {
            slot = a;
            break;
        }
        if (a->tick < slot->tick)
            slot = a;                           /* else the oldest (or a free one) */
    }
    if (slot->key == key && slot->tick == s_tickNo) {
        slot->dt += dt;
        return;
    }
    slot->key = key;
    slot->tick = s_tickNo;
    slot->dt = dt;
}

f32 pc_anim_offset(const void *key)
{
    if (g_pcViewLag <= 0.0f)
        return 0.0f;
    for (int i = 0; i < (int)(sizeof(s_anims) / sizeof(s_anims[0])); i++) {
        const PcAnim *a = &s_anims[i];
        if (a->key == key)
            return a->tick == s_tickNo ? -a->dt * g_pcViewLag : 0.0f;
    }
    return 0.0f;
}

/* ======================================================================== */
/* The values between ticks                                                 */
/* ======================================================================== */

/* A value at its last two ticks (as double: exact for f32 and int). */
struct PcTween {
    const void *owner;
    void *field;
    u32 tick;                   /* the tick of cur */
    u8 isInt;
    u8 kind;
    u8 have;                    /* 0 none, 1 cur, 2 prev and cur */
    u8 shown;                   /* the field holds `blend' (inside the owner's draw) */
    double prev, cur, blend;
    double lo, hi;              /* PC_TWEEN_LOOP: the loop */
};
static PcTween s_tweens[192];   /* a race display tracks 22 */

/* The owners being drawn and their nesting (a race display or two, the
 * cars of a view). */
enum { PC_TWEEN_OWNERS = 80 };
static const void *s_tweenOwner[PC_TWEEN_OWNERS];
static int s_tweenDepth[PC_TWEEN_OWNERS];

static PcTween *tween_slot(const void *owner, void *field)
{
    PcTween *oldest = &s_tweens[0];
    for (int i = 0; i < (int)(sizeof(s_tweens) / sizeof(s_tweens[0])); i++) {
        PcTween *t = &s_tweens[i];
        if (t->field == field && t->owner == owner)
            return t;
        if (t->field == NULL || t->tick < oldest->tick)
            oldest = t;
        if (t->field == NULL)
            break;
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->owner = owner;
    oldest->field = field;
    return oldest;
}

static PcTween *tween_note(const void *owner, void *field, int isInt, int kind, double v)
{
    PcTween *t = tween_slot(owner, field);

    if (t->have != 0 && t->tick == s_tickNo) {
        t->cur = v;                             /* again in the same tick */
    } else if (t->have != 0 && t->tick + 1 == s_tickNo) {
        t->prev = t->cur;
        t->cur = v;
        t->have = 2;
    } else {
        t->prev = t->cur = v;                   /* new, or not updated last tick */
        t->have = 1;
    }
    t->isInt = (u8)isInt;
    t->kind = (u8)kind;
    t->tick = s_tickNo;
    t->shown = 0;
    return t;
}

void pc_tween(const void *owner, f32 *value, int kind)
{
    tween_note(owner, value, 0, kind, *value);
}

void pc_tween(const void *owner, int *value, int kind)
{
    tween_note(owner, value, 1, kind, *value);
}

void pc_tween_loop(const void *owner, f32 *value, f32 lo, f32 hi)
{
    PcTween *t = tween_note(owner, value, 0, PC_TWEEN_LOOP, *value);
    t->lo = lo;
    t->hi = hi;
}

/* The step from prev to cur is one the kind blends. */
static int tween_continuous(const PcTween *t)
{
    double d = t->cur - t->prev;
    switch (t->kind) {
    case PC_TWEEN_ANY:
        return 1;
    case PC_TWEEN_LOOP:                         /* forward, or forward across the loop's end */
        return d >= 0.0 || (t->hi > t->lo && t->cur - (t->prev - (t->hi - t->lo)) >= 0.0);
    case PC_TWEEN_FADE:
        return d >= -0.1 && d <= 0.1;
    case PC_TWEEN_TIMER_UP:
        return d > 0.0 && d <= 2.5 / 60.0;
    case PC_TWEEN_MS:
        return t->prev >= 0.0 && d > 0.0 && d <= 2.5 * 1000.0 / 60.0;
    }
    return 0;
}

static double tween_get(const PcTween *t)
{
    return t->isInt ? (double)*(int *)t->field : (double)*(f32 *)t->field;
}

static void tween_set(PcTween *t, double v)
{
    if (t->isInt)
        *(int *)t->field = (int)(v < 0.0 ? v - 0.5 : v + 0.5);
    else
        *(f32 *)t->field = (f32)v;
}

void pc_tween_begin(const void *owner)
{
    int slot = -1;
    for (int i = 0; i < PC_TWEEN_OWNERS; i++) {
        if (s_tweenOwner[i] == owner && s_tweenDepth[i] > 0) {
            s_tweenDepth[i]++;                  /* nested: the outer pair blends */
            return;
        }
        if (slot < 0 && s_tweenDepth[i] == 0)
            slot = i;
    }
    if (slot < 0)
        return;                                 /* (never: a view's displays and cars) */
    s_tweenOwner[slot] = owner;
    s_tweenDepth[slot] = 1;
    if (g_pcViewLag <= 0.0f)
        return;
    double s = 1.0 - g_pcViewLag;               /* 0: the previous tick, 1: the last */
    for (int i = 0; i < (int)(sizeof(s_tweens) / sizeof(s_tweens[0])) && s_tweens[i].field != NULL; i++) {
        PcTween *t = &s_tweens[i];
        if (t->owner != owner || t->tick != s_tickNo || t->have != 2 || !tween_continuous(t))
            continue;
        if (tween_get(t) != (t->isInt ? (double)(int)t->cur : (double)(f32)t->cur))
            continue;                           /* changed since the tick (not the game's update) */
        double v = t->prev + (t->cur - t->prev) * s;
        if (t->kind == PC_TWEEN_LOOP && t->cur < t->prev) {
            const double span = t->hi - t->lo;  /* across the end: from prev unwrapped */
            const double p = t->prev - span;
            v = p + (t->cur - p) * s;
            if (v < t->lo)
                v += span;
        }
        tween_set(t, v);
        t->blend = tween_get(t);
        t->shown = 1;
    }
}

void pc_tween_end(const void *owner)
{
    int slot = -1;
    for (int i = 0; i < PC_TWEEN_OWNERS; i++) {
        if (s_tweenOwner[i] == owner && s_tweenDepth[i] > 0)
            slot = i;
    }
    if (slot < 0 || --s_tweenDepth[slot] > 0)
        return;
    s_tweenOwner[slot] = NULL;
    for (int i = 0; i < (int)(sizeof(s_tweens) / sizeof(s_tweens[0])) && s_tweens[i].field != NULL; i++) {
        PcTween *t = &s_tweens[i];
        if (t->owner != owner || !t->shown)
            continue;
        t->shown = 0;
        if (tween_get(t) == t->blend)           /* else the draw set it (a message started) */
            tween_set(t, t->cur);
    }
}
