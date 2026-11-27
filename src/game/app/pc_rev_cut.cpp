/* pc_rev_cut.cpp - the engines' rev limiter (pc_rev_cut.h).  No original:
 * new PC code. */
#include "game/app/pc_rev_cut.h"
#include "game/car/physics/T302_car_state.h"            /* RaceCarState, DynCarState, CarEngine */
#include "game/car/physics/T296_car_drivetrain.h"       /* car_engine_speed_from_wheels (PC 2026-11-09) */
#include "game/race/core/T124_race_information.h"       /* RaceInformation */
#include "game/race/core/T129_race_base.h"              /* RaceInfoBlock */
#include "game/race/core/T149_race_replay_information.h"    /* RaceReplayInformation */
#include "game/race/modes/T135_race_arcade_demo.h"      /* RaceArcadeDemoInformation */
#include "platform/settings.h"

#include <stdio.h>
#include <stdlib.h>                                     /* getenv, atoi (GT3PC_REV_CUT_LOG) */

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {

/* PC (2026-11-08): the lock of the two maps below.  User's crash (heap
 * corruption, 0xC0000374, at a 32-car race's start): the cars' own steps run
 * on the physics worker threads ([Debug] PhysicsThreads, auto by default;
 * game/race/dynamics/pc_physics_threads.h) and each car's first
 * engine_output inserted its cut into g_cuts - inserts on 15 threads at once
 * rehashed the table under each other.  The lock covers finding / making a
 * car's entry (a node: its address holds through rehashes); the cut itself
 * is the car's own, worked on by its own job alone, unlocked, so the cars'
 * order changes nothing.  (A cache of the last race looked up went too: two
 * threads tore it.) */
std::mutex g_lock;

/* The races with the realistic limiter (bound by race_organization_create
 * for every organization: it sets or clears the entry). */
std::unordered_map<const RaceOrganization *, bool> g_races;
/* PC (2026-11-20): the races playing a demo file's recording (a licence
 * test's, a time trial's: load_demo_setting) - the limiter it recorded,
 * realistic or not, in place of g_races' until a race not a replay starts
 * (pc_rev_cut_demo_end) */
std::unordered_map<const RaceOrganization *, bool> g_demos;

/* Each car's cut: the seconds of it left (0: none); the revs at the step
 * before and that step's fuel share; the rise of the last step with all its
 * fuel and of the last one with none (what tells where in a step the revs
 * reach the redline again: a step partly cut rises less than one with all
 * its fuel, which would run a whole step's rise past the redline unforeseen
 * - 280 rpm free-revving). */
struct Cut {
    f32 left = 0.0f;
    f32 run = 0.0f;                         /* PC (2026-11-09): the seconds since the cut began */
    f32 floor = 0.0f;                       /* PC (2026-11-08): the revs the cut ends at, its time left or not */
    int rpmPrev = 0;
    f32 fuelPrev = 1.0f;
    int riseFull = 0, riseCut = 0;
    bool havePrev = false, haveFull = false;
    int logAfter = 0;                       /* (GT3PC_REV_CUT_LOG: the steps after a cut still logged) */
};

/* PC (2026-11-08): GT3PC_REV_CUT_LOG=1 - each step of a player's car at the
 * limiter and the half second after a cut, a line each (the revs, the
 * step's fuel, the cut's time left and floor, the clutch, the gear, the
 * speed, the four wheels' tread speeds): how a real car bounces, for the
 * floor's and the time's tuning.  At most 4,000 lines a run (-1: the
 * variable not read yet, under g_lock). */
std::atomic<int> g_logLeft{-1};
std::unordered_map<const RaceCarState *, Cut> g_cuts;

/* A cut's length at rpm: PC_REV_CUT_CYCLES x 90 / rpm seconds (the user's
 * tuning, 2026-11-08, after 60 and 120: 68 ms at 8,000 rpm, 108 at 5,000). */
f32 cut_time(int rpm)
{
    return PC_REV_CUT_CYCLES * 90.0f / (f32)(rpm > 1 ? rpm : 1);
}

/* The revs a cut begun at rpm ends at, at the latest: as far below as the
 * engine alone falls in the cut's time (its friction on its own inertia -
 * the free revs' fall, GearCalc.inertiaInv the flywheel's) x
 * kFloorMargin.  User (2026-11-08): "low gear rev cuts ... 1st gear's
 * cutting springs the rpm back wildly ... Especially on wheelspin": with
 * the driven wheels spinning, the cut lets the slip collapse - the revs,
 * tied to the wheels, fall by all of it within the cut, and spring back as
 * the fuel spins the wheels up again.  In neutral, and in gear with the
 * car's inertia behind the engine, the revs fall as fast as the engine
 * alone or slower: the time ends the cut first, as tuned.  The harness (a
 * first gear on a saturating tyre, the wheels free to spin past the
 * engine's held redline + 10): the swing 504 -> 372 rpm at a 6,500 rpm
 * redline (neutral there: 316), neutral and top gear the same with the
 * floor as without. */
constexpr f32 kFloorMargin = PC_REV_CUT_FLOOR;
constexpr f32 kFloorLeast = 30.0f;                      /* rpm (an engine of no friction) */

/* PC (2026-11-08): a driven wheel spinning - its tread past the car's speed
 * by kSpinMin m/s and kSpinShare of it.  User (2026-11-08): "1st gear
 * bounce needs work: ... after the wheels have caught up (not spinning),
 * the cut rpm isn't at the revlimit, and the cut rate is super fast": a
 * gripping tyre's own slip (a few per cent: some 300 rpm at 6,500 in first)
 * collapses at a cut's start as well, past the floor - the cut ended at
 * once and the limiter buzzed.  So the floor is a spinning wheel's alone;
 * a cut with the wheels gripping lasts its time. */
constexpr f32 kSpinMin = 2.0f, kSpinShare = 0.15f;

bool wheelspin(const RaceCarState *car)
{
    const DynCarState *st = &car->st;
    const f32 v = st->speedX > 0.0f ? st->speedX : 0.0f;
    const f32 over = v * kSpinShare > kSpinMin ? v * kSpinShare : kSpinMin;
    for (int i = 0; i < 4; i++)
        if ((st->wheel[i].flags & WHEEL_DRIVEN) && st->wheel[i].spin - v > over)
            return true;
    return false;
}

/* PC (2026-11-08): the cap - a cut ends once the revs are PC_REV_CUT_CAP
 * below where it began, its time up or not.  User: "it drops rpm too hard
 * on cars that have a very light flywheel and strong enginebrake.  I think
 * there needs to be a cap, maybe 500rpm": the engine alone falls by about
 * k x 6 x 90 rpm in a cut whatever the rpm (k its fall rate, the engine
 * brake over the flywheel: 1.02 .. 4.65 /s in the database) - 550 rpm on
 * the slowest, 890 on the median, 2,500 on the fastest. */
f32 cut_floor(const RaceCarState *car, f32 rpm, f32 time)
{
    const f32 cap = rpm - PC_REV_CUT_CAP;
    if (!wheelspin(car))
        return cap;                                     /* (gripping, or free: the time, or the cap) */
    const f32 fall = car->st.frictionTorque * car->params.gear.inertiaInv * time * 9.5492966f;  /* rad/s -> rpm */
    const f32 depth = fall * kFloorMargin;
    const f32 spin = rpm - (depth > kFloorLeast ? depth : kFloorLeast);
    return spin > cap ? spin : cap;
}

/* A cut begun at rpm: its time (PC (2026-11-09): PC_REV_CUT_MIN_TIME at the
 * least) and its floor. */
void start_cut(Cut &c, const RaceCarState *car, f32 rpm)
{
    const f32 time = cut_time((int)rpm);
    c.left = time > PC_REV_CUT_MIN_TIME ? time : PC_REV_CUT_MIN_TIME;
    c.floor = cut_floor(car, rpm, c.left);
    c.run = 0.0f;
}

/* PC (2026-11-09): the engine's revs the limiter goes by - with the clutch
 * locked the wheels' (car_engine_speed_from_wheels, through a CVT /
 * clutchless box's ratio factor as car_engine_speed takes it): the game
 * holds the engine's own at the redline + 10 (car_engine_speed) while the
 * wheels may run past it, and the revs read there said nothing of how far
 * past.  Else the engine's (st.rpm: its own speed, free or slipping). */
int engine_rpm(RaceCarState *car)
{
    const DynCarState *st = &car->st;
    if (st->clutchState != CLUTCH_LOCKED)
        return st->rpm;
    f32 w = car_engine_speed_from_wheels(car);
    if (gearbox_is_ratio_glide(&car->params.gear))
        w *= car->cvtRatio;
    const int wheels = (int)(w * 9.5492966f);
    return wheels > st->rpm ? wheels : st->rpm;
}

/* A RaceInfoBlock's in its spare bytes (unk_080): 'R', then the version of
 * the realistic limiter, at +0x16 - the two bytes between the laps' part
 * (+0x10, 6 bytes) and the time's (+0x18): every block before this had 0
 * there (RaceBase::virtual_7 clears them).  Versions (all 2026-11-08): 1
 * (the first: 6 crank turns, no floor), 2 (6 cycles, the floor), 3 (the
 * floor a spinning wheel's alone, the redline foreseen in a step with the
 * clutch free alone), 4 (the cap: PC_REV_CUT_CAP below the cut's start at
 * the most), 5 (2026-11-09: PC_REV_CUT_MIN_TIME; in gear the wheels' revs,
 * no fuel in a step begun past the redline) - a race saved with another
 * replays with this one, logged. */
constexpr int kMarkAt = 0x16;
constexpr u8 kMark = 'R', kVersion = 5;

/* (both with g_lock held) */
void forget(const RaceOrganization *org)
{
    g_races.erase(org);
    g_demos.erase(org);                     /* PC (2026-11-20) */
}

/* The race's own limiter (the settings', or a saved race's) */
bool own_realistic(const RaceOrganization *org)
{
    auto it = g_races.find(org);
    return it != g_races.end() && it->second;
}

/* The limiter org's cars run: a demo file's while one plays (PC
 * 2026-11-20), else the race's own */
bool realistic(const RaceOrganization *org)
{
    auto demo = g_demos.find(org);
    if (demo != g_demos.end())
        return demo->second;
    return own_realistic(org);
}

/* The block's realistic version (0: the original) */
int block_version(const RaceInfoBlock *blk)
{
    return blk->unk_080[kMarkAt] == kMark ? blk->unk_080[kMarkAt + 1] : 0;
}

bool block_realistic(const RaceInfoBlock *blk)
{
    return block_version(blk) != 0;
}

void log_on(const char *what)
{
    /* (whole numbers: the game's printf, pdistd T533, is the original's -
     * its %g and %.0f are not the C library's) */
    const int cycles = (int)(PC_REV_CUT_CYCLES * 100.0f + 0.5f), margin = (int)(kFloorMargin * 100.0f + 0.5f);
    printf("PC: rev limiter: realistic (%s): a cut of %d.%02d cycles at the rpm it cuts at (%d ms at 8,000 rpm, %d at"
           " 5,000), ending once the revs are %d rpm down (with wheelspin: %d.%02d x as far as the engine alone would"
           " fall in it), %d ms at the least; in gear no fuel past the redline\n", what, cycles / 100, cycles % 100,
           (int)(cut_time(8000) * 1000.0f + 0.5f), (int)(cut_time(5000) * 1000.0f + 0.5f), (int)PC_REV_CUT_CAP,
           margin / 100, margin % 100, (int)(PC_REV_CUT_MIN_TIME * 1000.0f + 0.5f));
}

} // namespace

void pc_rev_cut_bind(RaceOrganization *org, RaceInformation *info)
{
    std::lock_guard<std::mutex> lock(g_lock);
    forget(org);
    if (org == NULL || info == NULL)
        return;
    if (dynamic_cast<RaceReplayInformation *>(info) != NULL || dynamic_cast<RaceArcadeDemoInformation *>(info) != NULL)
        return;                                         /* (a saved race's: pc_rev_cut_load) */
    if (settings::rev_cut() != settings::RevCut::realistic)
        return;
    g_races[org] = true;
    log_on("the settings'");
}

void pc_rev_cut_store(RaceInfoBlock *blk, const RaceOrganization *org)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (!realistic(org))
        return;
    blk->unk_080[kMarkAt] = kMark;
    blk->unk_080[kMarkAt + 1] = kVersion;
}

void pc_rev_cut_load(RaceOrganization *org, const RaceInfoBlock *blk)
{
    std::lock_guard<std::mutex> lock(g_lock);
    forget(org);
    if (org == NULL || blk == NULL || !block_realistic(blk))
        return;
    g_races[org] = true;
    log_on("the saved race's");
    if (block_version(blk) != kVersion)
        printf("PC: rev limiter: the race was saved with version %d of the realistic limiter, it replays with %d:"
               " it may drive otherwise\n", block_version(blk), kVersion);
}

void pc_rev_cut_demo(const RaceOrganization *org, const RaceInfoBlock *blk)
{
    if (org == NULL || blk == NULL)
        return;
    std::lock_guard<std::mutex> lock(g_lock);
    const bool demo = block_realistic(blk), own = own_realistic(org);
    g_demos[org] = demo;
    printf("PC: rev limiter: a demo file plays with the %s limiter it was recorded with (the race's own: %s)\n",
           demo ? "realistic" : "original", own ? "realistic" : "original");
    if (demo && block_version(blk) != kVersion)
        printf("PC: rev limiter: the demo was recorded with version %d of the realistic limiter, it plays with %d:"
               " it may drive otherwise\n", block_version(blk), kVersion);
}

void pc_rev_cut_demo_end(const RaceOrganization *org)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_demos.erase(org) != 0)
        printf("PC: rev limiter: the race's own again (%s)\n", own_realistic(org) ? "realistic" : "original");
}

void pc_rev_cut_check_ghost(const RaceInfoBlock *blk, const RaceOrganization *org)
{
    if (blk == NULL)
        return;
    std::lock_guard<std::mutex> lock(g_lock);
    const bool ghost = block_realistic(blk), race = realistic(org);
    if (ghost != race)
        printf("PC: rev limiter: the ghost was saved with the %s limiter, the race runs the %s: it may drive"
               " otherwise\n", ghost ? "realistic" : "original", race ? "realistic" : "original");
}

void pc_rev_cut_reset(RaceCarState *car)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_cuts.erase(car);
}

bool pc_rev_cut(RaceCarState *car, f32 *fuel)
{
    Cut *cut;
    {
        /* PC (2026-11-08): on any physics thread (g_lock above) */
        std::lock_guard<std::mutex> lock(g_lock);
        if (!realistic(car->org))
            return false;
        cut = &g_cuts[car];
        if (g_logLeft.load(std::memory_order_relaxed) < 0) {
            const char *env = getenv("GT3PC_REV_CUT_LOG");
            const int left = env != NULL && atoi(env) > 0 ? 4000 : 0;
            g_logLeft.store(left, std::memory_order_relaxed);
            if (left != 0)
                printf("PC: rev cut: GT3PC_REV_CUT_LOG=1: the players' cars' steps at the limiter are logged (rpm,"
                       " the step's fuel, the cut's time left, its floor rpm, the clutch, the gear, km/h, the wheels'"
                       " tread speeds FL FR RL RR)\n");
        }
    }
    Cut &c = *cut;
    DynCarState *st = &car->st;
    const CarEngine *e = &car->params.engine;
    const f32 dt = car->dt;
    const int rpm = engine_rpm(car), redline = e->redline;  /* PC (2026-11-09): in gear the wheels' */
    /* (the rises of the step that just ran, by its fuel) */
    if (c.havePrev && dt > 0.0f) {
        if (c.fuelPrev >= 1.0f) {
            c.riseFull = rpm - c.rpmPrev;
            c.haveFull = true;
        } else if (c.fuelPrev <= 0.0f) {
            c.riseCut = rpm - c.rpmPrev;
        }
    }
    f32 f = 1.0f;
    if (dt <= 0.0f) {
        f = c.left > 0.0f ? 0.0f : 1.0f;                /* (no time passes: as it is) */
    } else {
        /* the step's time in order: the cut under way to its end, then the
         * fuel until the revs reach the redline - where a step with all its
         * fuel rises them to, the last one's rise (the first's: this step
         * itself, as it began) - and a cut from there for its time, and so
         * on.  A step is 17 ms, a cut at 8,000 rpm 45: where it begins and
         * ends inside one is the timing.  PC (2026-11-08): the redline
         * foreseen within a step with the engine free alone (neutral, the
         * clutch in): in gear car_engine_speed holds the engine at the
         * redline + 10 - nothing runs past it to foresee - and the revs rise
         * by the tyres' slip coming back after a cut, steps faster than the
         * car takes them on: that rise foresaw the redline hundreds of rpm
         * early (user: "the cut rpm isn't at the revlimit").  In gear a cut
         * begins at a step at the redline */
        const bool free = st->clutchState == CLUTCH_FREE;
        const f32 upRate =
            free ? (f32)(c.haveFull ? c.riseFull : c.havePrev ? rpm - c.rpmPrev : 0) / dt : 0.0f;
        /* (rpm a second, cut: in gear the last step with no fuel's, 0 not
         * seen yet; PC (2026-11-08) free, the engine's own - its friction on
         * its flywheel, exact for a free engine with no fuel - as a light
         * one capped within its first step falls 600 rpm a step and no step
         * of it is all cut to measure) */
        const f32 downRate = free ? -st->frictionTorque * car->params.gear.inertiaInv * 9.5492966f
                                  : (f32)c.riseCut / dt;
        f32 t = 0.0f, fuelTime = 0.0f, r = (f32)rpm;
        if (!free && rpm >= redline) {
            /* PC (2026-11-09): in gear a step that begins at or past the
             * redline (the wheels' revs: engine_rpm) is cut all through - a
             * cut under way goes on, else one begins.  User: "Some cars can
             * cut so frequently that they can accelerate infinitely while
             * holding the redline": a cut ended inside a step by the
             * foreseen fall (the last all-cut step's, stale) or by the
             * floor gave the rest of the step its fuel while the wheels
             * were still past the redline, every step - the engine held at
             * the redline + 10 by the game, the wheels and the car going
             * on (the harness: 1st with wheelspin, the car past the
             * redline's speed).  GT3's own limiter stays cut until the
             * held revs are 500 below: never fuel past the redline. */
            if (c.left <= 0.0f)
                start_cut(c, car, r);
            c.left -= dt;
            c.run += dt;
            if (c.left < 0.0f)
                c.left = 0.0f;
            t = dt;
        }
        /* PC (2026-11-08): a cut whose revs are down to its floor ends
         * (cut_floor: the cap, the wheelspin's fall).  PC (2026-11-09): once
         * it has run PC_REV_CUT_MIN_TIME */
        if (c.left > 0.0f && r <= c.floor && c.run >= PC_REV_CUT_MIN_TIME)
            c.left = 0.0f;
        for (int pass = 0; pass < 8 && t < dt; pass++) {
            if (c.left > 0.0f) {
                f32 seg = c.left < dt - t ? c.left : dt - t;
                /* (the floor reached within the step, falling as the last
                 * step with no fuel did: the cut ends there, PC (2026-11-09)
                 * PC_REV_CUT_MIN_TIME into it at the soonest) */
                bool atFloor = false;
                if (downRate < 0.0f) {
                    f32 reach = (c.floor - r) / downRate;
                    const f32 soonest = PC_REV_CUT_MIN_TIME - c.run;
                    if (reach < soonest)
                        reach = soonest;
                    if (reach >= 0.0f && reach < seg) {
                        seg = reach;
                        atFloor = true;
                    }
                }
                c.left = atFloor ? 0.0f : c.left - seg;
                c.run += seg;
                t += seg;
                r += downRate * seg;
                continue;
            }
            if (r >= (f32)redline && r > 0.0f) {
                start_cut(c, car, r);                   /* (at or past it: a cut from here) */
                continue;
            }
            if (upRate > 0.0f) {
                const f32 reach = ((f32)redline - r) / upRate;
                if (t + reach < dt) {
                    fuelTime += reach;
                    t += reach;
                    r = (f32)redline;
                    start_cut(c, car, r);
                    continue;
                }
            }
            fuelTime += dt - t;
            t = dt;
        }
        f = fuelTime / dt;
        if (f > 1.0f)
            f = 1.0f;
    }
    if (c.left < 1e-6f)
        c.left = 0.0f;
    if (dt > 0.0f) {
        c.rpmPrev = rpm;
        c.fuelPrev = f;
        c.havePrev = true;
    }
    st->limiterOn = f < 1.0f ? 1 : 0;
    *fuel = f;
    /* PC (2026-11-08): GT3PC_REV_CUT_LOG (read above) */
    if (g_logLeft.load(std::memory_order_relaxed) > 0 && st->driver == DRIVER_PAD && dt > 0.0f) {
        if (f < 1.0f || c.left > 0.0f)
            c.logAfter = 30;
        if (c.logAfter > 0) {
            c.logAfter--;
            const int left = g_logLeft.fetch_sub(1, std::memory_order_relaxed) - 1;
            printf("PC: rev cut: %p rpm %d (redline %d) fuel %d%% left %d ms floor %d clutch %u gear %u %d km/h"
                   " wheels %d %d %d %d cm/s%s%s\n", (void *)car, rpm, redline, (int)(f * 100.0f + 0.5f),
                   (int)(c.left * 1000.0f + 0.5f), (int)c.floor, st->clutchState, st->gear, (int)(st->speedX * 3.6f),
                   (int)(st->wheel[0].spin * 100.0f), (int)(st->wheel[1].spin * 100.0f),
                   (int)(st->wheel[2].spin * 100.0f), (int)(st->wheel[3].spin * 100.0f),
                   wheelspin(car) ? " spinning" : "", left == 0 ? " (no more of these lines)" : "");
        }
    }
    return true;
}
