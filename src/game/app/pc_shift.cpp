/* pc_shift.cpp - the gear shifts (pc_shift.h).  No original: new PC code. */
#include "game/app/pc_shift.h"
#include "game/car/physics/T302_car_state.h"            /* RaceCarState, DynCarState, CarEngine, GearCalc */
#include "game/race/core/T124_race_information.h"       /* RaceInformation */
#include "game/race/core/T129_race_base.h"              /* RaceInfoBlock */
#include "game/race/core/T149_race_replay_information.h"    /* RaceReplayInformation */
#include "game/race/modes/T135_race_arcade_demo.h"      /* RaceArcadeDemoInformation */
#include "platform/settings.h"

#include <math.h>                                       /* powf */
#include <stdio.h>
#include <stdlib.h>                                     /* getenv, atoi (GT3PC_SHIFT_LOG) */

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {

/* PC (2026-11-08): the lock of the two maps below - the cars' steps run on
 * the physics worker threads ([Debug] PhysicsThreads,
 * game/race/dynamics/pc_physics_threads.h), and inserts on several threads
 * at once corrupt the heap (the user's crash, in pc_rev_cut.cpp's map, the
 * same code as here).  The lock covers finding / making a car's entry (a
 * node: its address holds through rehashes); the shift itself is the car's
 * own, worked on by its own job alone.  (A cache of the last race looked
 * up went too: two threads tore it.) */
std::mutex g_lock;

/* The races with the realistic shifts (bound by race_organization_create
 * for every organization: it sets or clears the entry): their
 * PC_SHIFT_RACE_* flags (PC 2026-11-09: the clutch mode, the H-shifters). */
std::unordered_map<const RaceOrganization *, int> g_races;
/* PC (2026-11-20): the races playing a demo file's recording (a licence
 * test's, a time trial's: load_demo_setting) - the flags it recorded (0: the
 * original) in place of g_races' until a race not a replay starts
 * (pc_shift_demo_end) */
std::unordered_map<const RaceOrganization *, int> g_demos;
/* PC (2026-11-09): the flags of the race last bound from the settings (a
 * live race: RaceInput::read's frames, pc_clutch_live_input) */
std::atomic<int> g_liveFlags{0};

/* A car's shift: none, the clutch out (its seconds left), the clutch
 * snapping in (its seconds left); the shift's time, for the log; a
 * downshift (PC 2026-11-08: the same timings, GT3's blip while out). */
enum Phase { PHASE_NONE, PHASE_OUT, PHASE_SNAP };
struct Shift {
    Phase phase = PHASE_NONE;
    f32 left = 0.0f;
    f32 time = 0.0f;
    int from = 0, to = 0;
    bool down = false;
    bool logged = false;                    /* (the car's first shift logged) */
};
std::unordered_map<const RaceCarState *, Shift> g_shifts;

/* GT3PC_SHIFT_LOG=1: each shift of a player's car, a line where the clutch
 * snaps in.  At most 2,000 lines a run (-1: the variable not read yet). */
std::atomic<int> g_logLeft{-1};

/* A RaceInfoBlock's in its spare bytes (unk_080): 'S', then the version of
 * the realistic shifts, at +0x1E - the two bytes between the time's part
 * (+0x18, 6 bytes) and the car multipliers' (+0x20, pc_car_tuning.cpp):
 * every block before this had 0 there (RaceBase::virtual_7 clears them).
 * Versions (2026-11-08): 1 (upshifts), 2 (downshifts too, every car's blip
 * metered), 3 (a player's downshift the player's own throttle), 4 (an
 * upshift quicker a gear) - a race saved with another replays with this
 * one, logged.  PC (2026-11-09): the version in the byte's low 6 bits, the
 * clutch mode bit 6 (kMarkClutch), the H-shifters bit 7 (kMarkHShifter):
 * the players' frames then hold their clutch and gates (pc_clutch.h).
 * PC (2026-11-17): 5 - the clutch mode's clutch holds the engine as the
 * power multiplier makes it (pc_clutch.cpp capacity; a race at 100 % power
 * drives as with 4). */
constexpr int kMarkAt = 0x1E;
constexpr u8 kMark = 'S', kVersion = 5;
constexpr u8 kMarkClutch = 0x40, kMarkHShifter = 0x80, kVersionMask = 0x3F;

constexpr f32 kRpmToRad = 0.10471976f;
constexpr f32 kRadToRpm = 9.5492966f;

/* (both with g_lock held) */
void forget(const RaceOrganization *org)
{
    g_races.erase(org);
    g_demos.erase(org);                     /* PC (2026-11-20) */
}

/* The race's own flags (the settings', or a saved race's) */
int own_flags(const RaceOrganization *org)
{
    auto it = g_races.find(org);
    return it != g_races.end() ? it->second : 0;
}

/* The flags org's cars run: a demo file's while one plays (PC 2026-11-20),
 * else the race's own */
int race_flags(const RaceOrganization *org)
{
    auto demo = g_demos.find(org);
    if (demo != g_demos.end())
        return demo->second;
    return own_flags(org);
}

bool realistic(const RaceOrganization *org)
{
    return (race_flags(org) & PC_SHIFT_RACE_REALISTIC) != 0;
}

int block_version(const RaceInfoBlock *blk)
{
    return blk->unk_080[kMarkAt] == kMark ? blk->unk_080[kMarkAt + 1] & kVersionMask : 0;
}

/* PC (2026-11-09): the block's flags (PC_SHIFT_RACE_*; 0: the original) */
int block_flags(const RaceInfoBlock *blk)
{
    if (block_version(blk) == 0)
        return 0;
    const u8 b = blk->unk_080[kMarkAt + 1];
    return PC_SHIFT_RACE_REALISTIC | ((b & kMarkClutch) ? PC_SHIFT_RACE_CLUTCH : 0) |
           ((b & kMarkHShifter) ? PC_SHIFT_RACE_HSHIFTER : 0);
}

bool block_realistic(const RaceInfoBlock *blk)
{
    return block_version(blk) != 0;
}

/* The shift behaviors by the gearbox type.  User (2026-11-08): "Keep stubs
 * for keying on a geartype: A sequential behavior will be added later." */
enum ShiftKind {
    SHIFT_GT3,                              /* the game's own */
    SHIFT_MANUAL,                           /* gear type 0: a manual gearbox, the clutch out for a time */
    SHIFT_SEQUENTIAL                        /* (stub: a sequential gearbox, later) */
};

ShiftKind shift_kind(const RaceCarState *car)
{
    if (car->hybrid)
        return SHIFT_GT3;                   /* (the Dualnote: its motors' drive, T344) */
    switch (car->params.gear.gearboxType) {
    case GEARBOX_STOCK:
        return SHIFT_MANUAL;                /* every GT3 gear part */
    /* (a sequential gear type: SHIFT_SEQUENTIAL, once the gear table's type
     * column has one - GEARBOX_*, T292_gear_calc.h) */
    default:
        return SHIFT_GT3;                   /* CVT, clutchless, electric: their own */
    }
}

/* The engine's fall rate, 1/s: its friction (torqueScale x the engine
 * speed, from the drivetrain's EngineBrake) over its inertia (the
 * drivetrain's FlywheelInertia), both after the parts' rates. */
f32 fall_rate(const RaceCarState *car)
{
    return car->params.engine.torqueScale * car->params.gear.inertiaInv;
}

/* The seconds the clutch is out on a shift from gear `from' (pc_shift.h's
 * formula).  PC (2026-11-08): an upshift PC_SHIFT_TIME_PER_GEAR quicker for
 * each gear above 1st it leaves (the user: "Each gear should get 0.01f
 * faster.  1st is the base it currently is"); a downshift the base. */
f32 shift_time(const RaceCarState *car, int from, bool down)
{
    f32 t = car->params.gear.gearKind == 0 ? PC_SHIFT_TIME_STOCK : PC_SHIFT_TIME_SPORT;
    if (!down && from > 1)
        t -= PC_SHIFT_TIME_PER_GEAR * (f32)(from - 1);
    const f32 k = fall_rate(car);
    if (k > 0.0f)
        t *= powf(PC_SHIFT_FALL_REF / k, PC_SHIFT_FALL_WEIGHT);
    return t < 0.02f ? 0.02f : t > 1.0f ? 1.0f : t;
}

/* (whole numbers and hundredths: the game's printf, pdistd T533, is the
 * original's - its %g and %.0f are not the C library's) */
int ms(f32 s)
{
    return (int)(s * 1000.0f + 0.5f);
}

void log_on(const char *what)
{
    const int ref = (int)(PC_SHIFT_FALL_REF * 100.0f + 0.5f), weight = (int)(PC_SHIFT_FALL_WEIGHT * 100.0f + 0.5f);
    printf("PC: gear shift: realistic (%s): a shift's clutch out %d ms (a stock gearbox part) or %d ms (the others),"
           " an upshift %d ms less for each gear above 1st, x (%d.%02d / the engine's fall rate) ^ %d.%02d, then"
           " snapped in over %d ms\n", what, ms(PC_SHIFT_TIME_STOCK), ms(PC_SHIFT_TIME_SPORT),
           ms(PC_SHIFT_TIME_PER_GEAR), ref / 100, ref % 100, weight / 100, weight % 100, ms(PC_SHIFT_SNAP));
}

bool log_each()
{
    if (g_logLeft.load(std::memory_order_relaxed) < 0) {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_logLeft.load(std::memory_order_relaxed) < 0) {
            const char *env = getenv("GT3PC_SHIFT_LOG");
            const int left = env != NULL && atoi(env) > 0 ? 2000 : 0;
            g_logLeft.store(left, std::memory_order_relaxed);
            if (left != 0)
                printf("PC: gear shift: GT3PC_SHIFT_LOG=1: each shift of a player's car is logged where its clutch"
                       " snaps in\n");
        }
    }
    return g_logLeft.load(std::memory_order_relaxed) > 0;
}

/* The shift of car under way (NULL: none, or not in CLUTCH_SHIFT any more -
 * GT3's own transitions end it: the lock, the clutch freed near
 * standstill).  Any physics thread (g_lock). */
Shift *active(RaceCarState *car)
{
    if (car->st.clutchState != CLUTCH_SHIFT)
        return NULL;
    std::lock_guard<std::mutex> lock(g_lock);
    if (!realistic(car->org))
        return NULL;
    auto it = g_shifts.find(car);
    if (it == g_shifts.end() || it->second.phase == PHASE_NONE)
        return NULL;
    return &it->second;
}

} // namespace

void pc_shift_bind(RaceOrganization *org, RaceInformation *info)
{
    std::lock_guard<std::mutex> lock(g_lock);
    forget(org);
    if (org == NULL || info == NULL)
        return;
    if (dynamic_cast<RaceReplayInformation *>(info) != NULL || dynamic_cast<RaceArcadeDemoInformation *>(info) != NULL)
        return;                                         /* (a saved race's: pc_shift_load) */
    g_liveFlags = 0;
    const settings::ShiftBehavior b = settings::shift_behavior();
    if (b == settings::ShiftBehavior::original)
        return;
    /* PC (2026-11-09): realistic + clutch, the H-shifters with it */
    int flags = PC_SHIFT_RACE_REALISTIC;
    if (b == settings::ShiftBehavior::clutch)
        flags |= PC_SHIFT_RACE_CLUTCH | (settings::h_shifter() ? PC_SHIFT_RACE_HSHIFTER : 0);
    g_races[org] = flags;
    g_liveFlags = flags;
    log_on("the settings'");
    if (flags & PC_SHIFT_RACE_CLUTCH)
        printf("PC: gear shift: the clutch mode - the players' manual gearboxes worked by their clutch%s\n",
               (flags & PC_SHIFT_RACE_HSHIFTER) ? ", their gears from their H-shifters" : "");
}

void pc_shift_store(RaceInfoBlock *blk, const RaceOrganization *org)
{
    std::lock_guard<std::mutex> lock(g_lock);
    const int flags = race_flags(org);
    if (!(flags & PC_SHIFT_RACE_REALISTIC))
        return;
    blk->unk_080[kMarkAt] = kMark;
    blk->unk_080[kMarkAt + 1] = (u8)(kVersion | ((flags & PC_SHIFT_RACE_CLUTCH) ? kMarkClutch : 0) |
                                     ((flags & PC_SHIFT_RACE_HSHIFTER) ? kMarkHShifter : 0));
}

void pc_shift_load(RaceOrganization *org, const RaceInfoBlock *blk)
{
    std::lock_guard<std::mutex> lock(g_lock);
    forget(org);
    if (org == NULL || blk == NULL || !block_realistic(blk))
        return;
    g_races[org] = block_flags(blk);
    log_on("the saved race's");
    if (block_flags(blk) & PC_SHIFT_RACE_CLUTCH)
        printf("PC: gear shift: the saved race's clutch mode%s\n",
               (block_flags(blk) & PC_SHIFT_RACE_HSHIFTER) ? " with the H-shifters" : "");
    if (block_version(blk) != kVersion)
        printf("PC: gear shift: the race was saved with version %d of the realistic shifts, it replays with %d: it"
               " may drive otherwise\n", block_version(blk), kVersion);
}

/* (the flags' name for the log lines) */
static const char *flags_name(int flags)
{
    if (!(flags & PC_SHIFT_RACE_REALISTIC))
        return "original";
    if (!(flags & PC_SHIFT_RACE_CLUTCH))
        return "realistic";
    return (flags & PC_SHIFT_RACE_HSHIFTER) ? "realistic + clutch, H-shifters" : "realistic + clutch";
}

void pc_shift_demo(const RaceOrganization *org, const RaceInfoBlock *blk)
{
    if (org == NULL || blk == NULL)
        return;
    std::lock_guard<std::mutex> lock(g_lock);
    const int demo = block_flags(blk), own = own_flags(org);
    g_demos[org] = demo;
    printf("PC: gear shift: a demo file plays with the %s shifts it was recorded with (the race's own: %s)\n",
           flags_name(demo), flags_name(own));
    if (demo != 0 && block_version(blk) != kVersion)
        printf("PC: gear shift: the demo was recorded with version %d of the realistic shifts, it plays with %d: it"
               " may drive otherwise\n", block_version(blk), kVersion);
}

void pc_shift_demo_end(const RaceOrganization *org)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_demos.erase(org) != 0)
        printf("PC: gear shift: the race's own again (%s)\n", flags_name(own_flags(org)));
}

void pc_shift_check_ghost(const RaceInfoBlock *blk, const RaceOrganization *org)
{
    if (blk == NULL)
        return;
    std::lock_guard<std::mutex> lock(g_lock);
    const bool ghost = block_realistic(blk), race = realistic(org);
    if (ghost != race)
        printf("PC: gear shift: the ghost was saved with the %s shifts, the race runs the %s: it may drive"
               " otherwise\n", ghost ? "realistic" : "original", race ? "realistic" : "original");
}

void pc_shift_reset(RaceCarState *car)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_shifts.erase(car);
}

int pc_shift_race_flags(const RaceOrganization *org)
{
    std::lock_guard<std::mutex> lock(g_lock);
    return race_flags(org);
}

int pc_shift_live_flags(void)
{
    return g_liveFlags.load(std::memory_order_relaxed);
}

void pc_shift_begin(RaceCarState *car, int from, int to, bool wasShift)
{
    Shift *found;
    {
        /* PC (2026-11-08): on any physics thread (g_lock above) */
        std::lock_guard<std::mutex> lock(g_lock);
        if (!realistic(car->org))
            return;
        found = &g_shifts[car];
    }
    Shift &s = *found;
    switch (shift_kind(car)) {
    case SHIFT_MANUAL:
        break;
    case SHIFT_SEQUENTIAL:                  /* (stub: the original's until it is written) */
    case SHIFT_GT3:
    default:
        s.phase = PHASE_NONE;
        return;
    }
    const DynCarState *st = &car->st;
    const u8 transMode = (st->driver != 1 && st->ctrlMode == 0) ? st->transMode : 0;
    if (transMode == 3 || transMode == 6) {
        s.phase = PHASE_NONE;               /* (the player's clutch) */
        return;
    }
    if (wasShift && s.phase == PHASE_OUT) {
        s.to = to;                          /* (a gear more with the clutch out: its time goes on, */
        s.down = to < s.from;               /* the way from the gear it left) */
        return;
    }
    s.phase = PHASE_OUT;
    s.down = to < from;                     /* PC (2026-11-08): a downshift the same timings */
    s.time = s.left = shift_time(car, from, s.down);
    s.from = from;
    s.to = to;
    if (!s.logged && st->driver == DRIVER_PAD) {
        s.logged = true;
        const int k = (int)(fall_rate(car) * 100.0f + 0.5f);
        printf("PC: gear shift: car %p (a player's): gearbox part kind %u, the engine's fall rate %d.%02d/s: the clutch"
               " out %d ms on its first shift (%d -> %d; an upshift %d ms less for each gear above 1st)\n", (void *)car,
               car->params.gear.gearKind, k / 100, k % 100, ms(s.time), from, to, ms(PC_SHIFT_TIME_PER_GEAR));
    }
}

bool pc_shift_throttle(RaceCarState *car)
{
    Shift *s = active(car);
    if (s == NULL)
        return false;
    DynCarState *st = &car->st;
    if (s->phase == PHASE_OUT) {
        if (!s->down) {
            st->throttleOut = 0.0f;         /* (an upshift: the foot off) */
            return true;
        }
        /* PC (2026-11-08) a player's downshift (DRIVER_PAD, driving it - a
         * replay plays its throttle back the same): the clutch alone, the
         * blip the player's own throttle.  User: "the player should provide
         * the blip.  The downshift should only do the clutch part, so if no
         * throttle is applied you get an engine brake behavior instead": no
         * throttle, the revs fall on the engine brake and the clutch snaps
         * them up to the new gear's, a tug */
        if (st->driver == DRIVER_PAD && st->ctrlMode == CTRL_DRIVER)
            return true;
        /* the computer's (no blip of its own): GT3's blip (car_clutch_state:
         * full while the engine is below the new gear's speed, off above
         * it; GT3 locks where they meet), metered - the throttle that takes
         * the engine to the new gear's speed within the step (engine_output:
         * the torque is load x (engineTorque + frictionTorque) -
         * frictionTorque), so the revs are brought up and held there until
         * the clutch snaps in.  Full or nothing held for the clutch's time
         * sawed them 300 - 450 rpm about it at 15 - 20 Hz (the harness) */
        f32 load = !(st->wheelEngineW < st->engineWPrev) ? 1.0f : 0.0f;
        const f32 inertiaInv = car->params.gear.inertiaInv, span = st->engineTorque + st->frictionTorque;
        if (car->dt > 0.0f && inertiaInv > 0.0f && span > 0.0f) {
            const f32 need = (st->wheelEngineW - st->engineWPrev) / (inertiaInv * car->dt);
            load = (need + st->frictionTorque) / span;
            load = load < 0.0f ? 0.0f : load > 1.0f ? 1.0f : load;
        }
        st->throttleOut = load;
    }
    return true;                            /* (snapping in: the driver's own) */
}

bool pc_shift_drive(RaceCarState *car, f32 engineTq, f32 *drive)
{
    Shift *s = active(car);
    if (s == NULL)
        return false;
    if (s->phase == PHASE_OUT) {
        *drive = 0.0f;                      /* (the clutch out: nothing either way) */
        return true;
    }
    /* snapping in: the torque that takes the engine to the wheels' speed in
     * the snap's time left (all of the gap at its last step) - the engine's
     * momentum through the drivetrain, which pushes the wheels on (the
     * engine above them) or holds them back (below) */
    const DynCarState *st = &car->st;
    const f32 inertiaInv = car->params.gear.inertiaInv;
    f32 left = s->left > car->dt ? s->left : car->dt;
    *drive = engineTq;
    if (inertiaInv > 0.0f && left > 0.0f)
        *drive += (st->engineSpeed - st->wheelEngineW) / (inertiaInv * left);
    return true;
}

int pc_shift_engine(RaceCarState *car)
{
    Shift *s = active(car);
    if (s == NULL)
        return PC_SHIFT_GT3;
    DynCarState *st = &car->st;
    const f32 dt = car->dt;
    if (s->phase == PHASE_OUT) {
        const f32 idle = (f32)car->params.engine.idle * kRpmToRad;
        if (st->engineSpeed < idle)
            st->engineSpeed = idle;         /* (a free engine's floor) */
        if (dt > 0.0f)
            s->left -= dt;
        if (s->left <= 1e-4f) {
            s->phase = PHASE_SNAP;
            s->left = PC_SHIFT_SNAP;
            if (st->driver == DRIVER_PAD && log_each()) {
                const int left = g_logLeft.fetch_sub(1, std::memory_order_relaxed) - 1;
                const int engine = (int)(st->engineSpeed * kRadToRpm), wheels = (int)(st->wheelEngineW * kRadToRpm);
                printf("PC: gear shift: %p %u -> %u after %d ms: the engine %d rpm, the gear's %d (%s %d)%s\n",
                       (void *)car, s->from, s->to, ms(s->time), engine, wheels,
                       engine >= wheels ? "above by" : "below by", engine >= wheels ? engine - wheels : wheels - engine,
                       left == 0 ? " (no more of these lines)" : "");
            }
        }
        return PC_SHIFT_HOLD;
    }
    if (dt > 0.0f)
        s->left -= dt;
    if (s->left <= 1e-4f) {
        s->phase = PHASE_NONE;
        return PC_SHIFT_ENGAGE;
    }
    return PC_SHIFT_GT3;
}
