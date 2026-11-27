/* pc_shift.h - PC (ROADMAP 64, no original): the gear shifts, original or
 * realistic ("gear shift behavior": [Game] ShiftBehavior;
 * settings::shift_behavior).
 *
 * User request (2026-11-08): "Gear shift behavior setting: Original or
 * Realistic.  Realistic keys on the gear type values in the gear table. ...
 * this setting will only apply to gear type 0, which in the case of retail
 * GT3 data is ALL gear parts because there is no gear type column. ... Type
 * 0 is a standard manual gearbox behavior.  The original behavior is a wait
 * for the rpm to fall until it matches the queued gear for shifting up.
 * Shifting down in simple, usually an instant rpm bring-up and engage.  With
 * realistic, we want a time-based formula that engages the shift on either
 * side of the rpm fall, either before the line up or after it fell past.
 * Formula: Use the clutch / flywheel data: iflywheel and enginebrake values
 * already determine the general shifting speed by affecting the rpm fall off
 * rate.  The blend of those 2 values together provide the per-car dynamic
 * variance.  The gear part's category value should play as the main factor
 * in the formula: 0 (a stock gearbox) should have the slowest modifier, and
 * categories greater than [0] have the faster modifier.  Keep stubs for
 * keying on a geartype: A sequential behavior will be added later."  Then:
 * "GT3's fixed clutch behavior might be undesirable for the mode, as it
 * slips pretty slowly.  We'll want a hard clutch release that gives a fast
 * snap into the rpm".  After tuning the times: "Downshifts should mirror
 * the same timings too for realistic."  Then: "Instead, the player should
 * provide the blip.  The downshift should only do the clutch part, so if no
 * throttle is applied you get an engine brake behavior instead."  PC
 * (2026-11-09): a third value, Realistic + Clutch - these shifts for every
 * car but the players' manual gearboxes, which their own clutch works
 * (game/app/pc_clutch.h).
 *
 * Original (GT3's, game/car/physics T296 / T344): an upshift puts the clutch
 * in CLUTCH_SHIFT - the throttle shut, the clutch passing
 * GearCalc.clutchTorque, which is CarSpec +0xCD x 9.8: a byte nothing writes
 * (the clutch part's ClutchTorque goes to +0xCC), so 0 in every race - and
 * the engine falls on its own friction until it meets the new gear's speed,
 * where the clutch locks.  A downshift blips the throttle full until the
 * engine meets the wheels' speed.
 *
 * Realistic, a shift of a manual gearbox (GearCalc.gearboxType 0, not the
 * Dualnote): the clutch is out (no torque either way; an upshift's throttle
 * shut, the engine falling on its own friction, at least at idle) for a
 * time
 *
 *     T = (PC_SHIFT_TIME_STOCK or _SPORT (the GEAR part's Kind 0, or 1..3)
 *          - PC_SHIFT_TIME_PER_GEAR x (the gear shifted from - 1), upshifts)
 *         x (PC_SHIFT_FALL_REF / k) ^ PC_SHIFT_FALL_WEIGHT
 *
 * (user, 2026-11-08: "More upshift behavior: Each gear should get 0.01f
 * faster.  1st is the base it currently is, 2nd gets faster and so on":
 * 1st -> 2nd the base, 2nd -> 3rd 0.01 s less, 3rd -> 4th 0.02 less...)
 *
 * k the engine's fall rate (1/s: the engine brake on the flywheel - its
 * friction, CarEngine.torqueScale x the engine speed, over GearCalc.inertia,
 * both after the flywheel / clutch / shaft parts' rates: a free engine with
 * the throttle shut falls as e^-kt), then snaps in wherever the revs are:
 * the clutch takes the engine to the wheels' speed in PC_SHIFT_SNAP seconds
 * (the torque that needs, the engine's momentum through the drivetrain -
 * above the new gear's revs a push, below them a tug), locking where they
 * meet.  T hardly depends on the gear: a big step (1st to 2nd) engages
 * before the revs are down to the new gear's, a small one after they fell
 * past.  A downshift takes the same T (the base's, whatever the gear) and
 * snap, the clutch alone: a
 * player's car (DRIVER_PAD, driving it) has the player's own throttle - a
 * blip brings the revs up, none lets them fall on the engine brake and the
 * snap pulls them up to the new gear's (a tug, the engine braking the
 * wheels).  A computer car has no blip of its own: GT3's (full below the
 * new gear's speed, off above; GT3 locks where they meet), metered - the
 * throttle that reaches the new gear's speed within a step, then holds it
 * as the car slows - so it snaps in lined up, T later than GT3's.  The
 * transmission modes with the player's clutch (3, 6) and the other
 * gearbox types stay GT3's; a sequential type has a stub (pc_shift.cpp
 * shift_kind).
 *
 * A race takes the setting when its organization is made
 * (race_organization_create -> pc_shift_bind) and keeps it.  A replay runs
 * the cars' physics again from their recorded inputs, so a saved race
 * records realistic (its RaceInfoBlock's spare bytes: 'S', 4 at +0x1E, after
 * the time's part, game/app/pc_course_pick.cpp) and its replay runs it (none
 * recorded: original).  The attract mode's demos run the original; a demo
 * file (a licence test's, a time trial's) the shifts it recorded (PC
 * 2026-11-20: pc_shift_demo).  A ghost
 * runs in its race's (a ghost saved with the other one is logged:
 * pc_shift_check_ghost).  A car's shift is its own, from DynCar_resetState
 * (T330) on.
 *
 * The log: "PC: gear shift: realistic (...)", a player's car's time at its
 * first shift; GT3PC_SHIFT_LOG=1 every player's shift (the engine's and the
 * new gear's rpm where the clutch snaps in). */
#ifndef GAME_APP_PC_SHIFT_H
#define GAME_APP_PC_SHIFT_H

#include "types.h"

class RaceCarState;
class RaceInformation;
class RaceOrganization;
struct RaceInfoBlock;

/* The seconds the clutch is out on a shift for the median car (k =
 * PC_SHIFT_FALL_REF): the GEAR part's Kind 0 (stock), 1 .. 3 (close, super
 * close, fully customisable).  The user's tuning (2026-11-08; built with
 * 0.20 / 0.12 and a 0.05 s snap). */
#define PC_SHIFT_TIME_STOCK 0.24f
#define PC_SHIFT_TIME_SPORT 0.20f
/* The seconds an upshift gets quicker for each gear above 1st it is shifted
 * from (1st -> 2nd: the base above; the user, 2026-11-08). */
#define PC_SHIFT_TIME_PER_GEAR 0.03f
/* The median engine fall rate, 1/s (paramdb_us: 283 drivetrains with their
 * engines' last curve rpm, 1.02 .. 4.65, 10 % 1.15, 90 % 3.70), and how much
 * a car's own moves the time: x (ref / k) ^ weight (0: not at all; 1: every
 * car falls the same share in its time). */
#define PC_SHIFT_FALL_REF 1.65f
#define PC_SHIFT_FALL_WEIGHT 0.5f
/* The seconds the clutch takes to snap the engine to the wheels' speed. */
#define PC_SHIFT_SNAP 0.10f

/* race_organization_create (before setupOrganization): org's shifts from
 * the settings (a replay's or a demo's: the original). */
void pc_shift_bind(RaceOrganization *org, RaceInformation *info);
/* RaceBase::virtual_7: org's shifts into blk (realistic only). */
void pc_shift_store(RaceInfoBlock *blk, const RaceOrganization *org);
/* RaceReplayInformation::setupOrganization: the shifts blk recorded. */
void pc_shift_load(RaceOrganization *org, const RaceInfoBlock *blk);
/* RaceBase::virtual_19 (a ghost into a running race): logged when blk was
 * recorded with the other shifts than org's. */
void pc_shift_check_ghost(const RaceInfoBlock *blk, const RaceOrganization *org);
/* PC (2026-11-20): load_demo_setting (T102): a demo file's recording (a
 * licence test's, a time trial's) plays on the session's own race, whose
 * shifts came from the settings - org's cars run the shifts blk recorded
 * (none: the original; its clutch mode and H-shifters too) instead, until
 * RaceBase::virtual_8 starts a race that is not a replay (pc_shift_demo_end:
 * the race's own again).  The user: the demos drove otherwise with Gear
 * shift behavior on Realistic. */
void pc_shift_demo(const RaceOrganization *org, const RaceInfoBlock *blk);
void pc_shift_demo_end(const RaceOrganization *org);

/* DynCar_resetState: car's shift over. */
void pc_shift_reset(RaceCarState *car);

/* PC (2026-11-09): a race's flags - the realistic shifts, the clutch mode
 * ([Game] ShiftBehavior=clutch: game/app/pc_clutch.h), the H-shifters
 * ([Game] HShifter); bound from the settings or the saved race's mark (its
 * byte's bits 6 / 7) - org's, and the race last bound from the settings'
 * (the live race's frames: RaceInput::read).  Any thread. */
enum { PC_SHIFT_RACE_REALISTIC = 1, PC_SHIFT_RACE_CLUTCH = 2, PC_SHIFT_RACE_HSHIFTER = 4 };
int pc_shift_race_flags(const RaceOrganization *org);
int pc_shift_live_flags(void);

/* car_gear_select: a forward -> forward change from gear `from` to `to` that
 * put the clutch in CLUTCH_SHIFT; wasShift: it was in CLUTCH_SHIFT before
 * (a gear more while the clutch is out keeps its time). */
void pc_shift_begin(RaceCarState *car, int from, int to, bool wasShift);
/* car_clutch_state, CLUTCH_SHIFT: true when the realistic shift has the
 * throttle (while the clutch is out an upshift's shut, a player's
 * downshift the player's, a computer car's downshift the metered blip; the
 * driver's as it snaps in). */
bool pc_shift_throttle(RaceCarState *car);
/* car_drive_torque, CLUTCH_SHIFT: true with the clutch's torque, signed
 * (positive: the engine drives the wheels). */
bool pc_shift_drive(RaceCarState *car, f32 engineTq, f32 *drive);
/* car_engine_speed, CLUTCH_SHIFT: the realistic shift's step - PC_SHIFT_GT3
 * (none, or the snap under way: GT3's lock where the engine and the wheels
 * meet), PC_SHIFT_HOLD (the clutch out: no lock; the engine kept at least at
 * idle), PC_SHIFT_ENGAGE (the snap's time is up: lock). */
enum { PC_SHIFT_GT3 = 0, PC_SHIFT_HOLD = 1, PC_SHIFT_ENGAGE = 2 };
int pc_shift_engine(RaceCarState *car);

#endif /* GAME_APP_PC_SHIFT_H */
