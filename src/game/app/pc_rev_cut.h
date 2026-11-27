/* pc_rev_cut.h - PC (ROADMAP 63, no original): the engines' rev limiter,
 * original or realistic ("rev cut behavior": [Game] RevCut; settings::rev_cut).
 *
 * User request (2026-11-08): "a new Extras setting: Rev cut behavior:
 * Original or Realistic.  Realistic setting is time-based instead of the
 * static 500 rpm step.  Factors: Time and rpm.  Revs cutting at a higher rpm
 * will have a more granular cut time, and cutting at a lower rpm will have it
 * last longer."
 *
 * Original (GT3's engine_output, game/car/physics T338): at the redline
 * (CarEngine.redline) the fuel is cut - the engine gives its friction alone -
 * until the revs are 500 rpm below it.  In a high gear the car slows the
 * whole 500 rpm before the drive comes back: a surge.
 *
 * Realistic: at the redline the fuel is cut for the time PC_REV_CUT_CYCLES
 * four-stroke cycles take at the rpm it cut at - 6 x 90 / rpm seconds (the
 * user's tuning, 2026-11-08): 68 ms at 8,000 rpm, 108 ms at 5,000 - then
 * given back; the revs at the redline again, another cut.  A physics step
 * (1/60 s x the race's time scale, DynCarState.timeScale) that the cut takes
 * part of gives the rest of its torque (the fuel's share of the step), so
 * the cut keeps its time at any rpm rather than whole steps: in neutral the
 * revs bounce off the redline, faster the higher it is; in gear the car
 * holds it.  A cut begun with a driven wheel spinning also ends once the
 * revs have fallen 1.25 x as far as the engine alone falls in its time (its
 * friction on its flywheel): the cut lets the slip collapse and the revs,
 * tied to the wheels, fall by all of it - in a low gear they sprang back
 * (user report, new_beetle_00 in first gear).  A gripping tyre's own slip
 * collapses too, and a floor on it ended the cuts at once (the limiter
 * buzzed): with the wheels gripping, and in neutral, a cut lasts its time.
 * Any cut also ends once the revs are PC_REV_CUT_CAP (500) rpm below where
 * it began: a free engine falls some k x 540 rpm in a cut whatever the rpm
 * (k its engine brake over its flywheel, 1.02 .. 4.65 /s) - 2,500 rpm on a
 * light flywheel with a strong engine brake (user report, 2026-11-08).
 * PC (2026-11-09): a cut lasts PC_REV_CUT_MIN_TIME (30 ms) at the least,
 * the cap or the floor notwithstanding (light engines cut 27 - 29 times a
 * second); in gear the limiter goes by the wheels' revs (the game holds the
 * engine's at the redline + 10 while the wheels may run past it), and a step
 * begun at or past the redline there gets no fuel at all - a cut ended
 * inside such a step gave the rest of it its fuel, and a car could gain
 * speed for ever with the revs held at the redline (user report).
 * The redline is foreseen within a step with the clutch free alone; in gear
 * the game holds the engine at the redline + 10 and a cut begins at a step
 * at the redline.  DynCarState.limiterOn is 1
 * for a step the cut takes any of (the HUD's shift lamp, which the revs past
 * the shift point light anyway).
 *
 * A race takes the setting when its organization is made
 * (race_organization_create -> pc_rev_cut_bind) and keeps it.  A replay runs
 * the cars' physics again from their recorded inputs, so a saved race
 * records realistic (its RaceInfoBlock's spare bytes: 'R', 5 at +0x16, between
 * the laps' and the time's parts, game/app/pc_course_pick.cpp) and its
 * replay runs it (none recorded: original).  The attract mode's demos run the
 * original; a demo file (a licence test's, a time trial's) the limiter it
 * recorded (PC 2026-11-20: pc_rev_cut_demo).  A ghost runs in its race's (a ghost saved with the other one is
 * logged: pc_rev_cut_check_ghost).  The cut's time left is the car's own,
 * from DynCar_resetState (T330) on.
 *
 * The log: "PC: rev limiter: realistic (...)". */
#ifndef GAME_APP_PC_REV_CUT_H
#define GAME_APP_PC_REV_CUT_H

#include "types.h"

class RaceCarState;
class RaceInformation;
class RaceOrganization;
struct RaceInfoBlock;

/* The cycles a realistic cut lasts: 6 x 90 / rpm seconds (the user's tuning,
 * 2026-11-08; the constant 90 in pc_rev_cut.cpp cut_time). */
#define PC_REV_CUT_CYCLES 6.0f
/* A cut begun with a driven wheel spinning ends once the revs have fallen
 * this many times as far as the engine alone falls in the cut's time
 * (pc_rev_cut.cpp cut_floor): lower, the wheelspin's bounces are cut
 * shorter. */
#define PC_REV_CUT_FLOOR 1.25f
/* PC (2026-11-08): a cut ends once the revs are this many rpm below where it
 * began, its time up or not (the user: "it drops rpm too hard on cars that
 * have a very light flywheel and strong enginebrake.  I think there needs to
 * be a cap, maybe 500rpm"). */
#define PC_REV_CUT_CAP 500.0f
/* PC (2026-11-09): the seconds a cut lasts at the least - neither the cap
 * nor the wheelspin's floor ends it sooner (the user: "Some cars cut too
 * frequently, I suggest a minimum time as well as the cap from earlier"). */
#define PC_REV_CUT_MIN_TIME 0.030f

/* race_organization_create (before setupOrganization): org's limiter from
 * the settings (a replay's or a demo's: the original). */
void pc_rev_cut_bind(RaceOrganization *org, RaceInformation *info);
/* RaceBase::virtual_7: org's limiter into blk (realistic only). */
void pc_rev_cut_store(RaceInfoBlock *blk, const RaceOrganization *org);
/* RaceReplayInformation::setupOrganization: the limiter blk recorded. */
void pc_rev_cut_load(RaceOrganization *org, const RaceInfoBlock *blk);
/* RaceBase::virtual_19 (a ghost into a running race): logged when blk was
 * recorded with the other limiter than org's. */
void pc_rev_cut_check_ghost(const RaceInfoBlock *blk, const RaceOrganization *org);
/* PC (2026-11-20): load_demo_setting (T102): a demo file's recording (a
 * licence test's, a time trial's) plays on the session's own race, whose
 * limiter came from the settings - org's cars run the limiter blk recorded
 * (none: the original) instead, until RaceBase::virtual_8 starts a race that
 * is not a replay (pc_rev_cut_demo_end: the race's own again).  The user:
 * the demos drove otherwise with Rev cut behavior on Realistic. */
void pc_rev_cut_demo(const RaceOrganization *org, const RaceInfoBlock *blk);
void pc_rev_cut_demo_end(const RaceOrganization *org);

/* DynCar_resetState: car's cut over. */
void pc_rev_cut_reset(RaceCarState *car);

/* engine_output: for a car whose race has the realistic limiter, true with
 * the step's fuel share (0 .. 1: the part of the step not cut), limiterOn
 * set; false for the original's (engine_output's own). */
bool pc_rev_cut(RaceCarState *car, f32 *fuel);

#endif /* GAME_APP_PC_REV_CUT_H */
