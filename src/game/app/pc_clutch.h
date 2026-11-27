/* pc_clutch.h - PC (ROADMAP 64, no original): the realistic gear shift's
 * clutch mode ([Game] ShiftBehavior=clutch, "Realistic + Clutch"; [Game]
 * HShifter): the players' manual gearboxes worked by their own clutch.
 *
 * User request (2026-11-09): "Gear Shift behavior: Clutch mode 'Realistic +
 * Clutch'.  Clutch mode being on reveals an H-Shifter setting: On or off. ...
 * Clutch design: Analog.  85% travel counts as fully disengaged.  The analog
 * modulation of the clutch controls the shift_snap behavior.  While in
 * clutch mode, the shift speed is directly based on the player's inputs.
 * Mis-shifting behavior will have to be designed, should behave like a
 * penalty / slow shift.  Most prominently, shifting without reaching the 85%
 * clutch threshold."
 *
 * Whose: a player's car (DynCarState.driver DRIVER_PAD, driving it), with a
 * manual gearbox (the game's MT: transmission mode TRANS_SEMI_AUTO, 1 - the
 * entry's transmission is the options' gearAuto ^ 1 -; also TRANS_MANUAL and
 * TRANS_SEQUENTIAL, which no menu sets), a gear type 0 gearbox (not the
 * Dualnote), not held (held at the start - the countdown - only its gears
 * are the clutch mode's, PC 2026-11-10: the gate or the shift buttons, a gear
 * in as it is taken, the game having the clutch out), not in the
 * pit, while its frame holds a clutch (the player's controller or wheel
 * maps one: platform/clutch_input.h).  Every other car - the computer's, an
 * automatic, a player without a clutch mapped - shifts as the realistic
 * mode does (pc_shift.h).
 *
 * The clutch: the pedal's travel p (0 released .. 1 pressed); from
 * PC_CLUTCH_DISENGAGED (85 %) on the clutch is out, below it it grips e = 1
 * - p / 0.85 of its whole: it passes at most e x PC_CLUTCH_CAPACITY x the
 * engine's peak torque (PC 2026-11-17: x the car's power multiplier,
 * pc_car_tuning.h - sized for the engine as tuned; at 1000 % a stock-sized
 * clutch let out on a flared engine never locked), the torque that brings the engine to the wheels'
 * speed within the step when that is less (it locks where they meet), else
 * all of it, slipping.  Locked, it slips again once the drive passes what
 * it holds (a pedal half let out under full throttle).  So the pedal's speed
 * is the shift's: let out at once, the engine snaps to the wheels (a push
 * above the new gear's revs, a tug below them); let out slowly, it slides
 * in; and a start from rest is the player's (GT3's own clutch slip and its
 * frees are not used).  The engine never falls below its idle there (it
 * cannot stall): at its idle the clutch passes no more than the engine's
 * own torque, and below the idle's wheel speed it does not lock.
 *
 * The gears: the player's shift buttons (GT3's manual box: car_select_gear)
 * or, with the H-shifters on and the player's H-Shifter mapped, its gate (the
 * reverse its Reverse gate, or the game's reverse control while it is held;
 * a gate the car has no gear for, neutral; neutral frees the clutch, the
 * gear shown kept).  The reverse is a gear (user, 2026-11-09: "In clutch
 * mode reverse behavior needs to be changed to a realistic shift to reverse
 * then use the throttle"): GT3's MT (TRANS_SEMI_AUTO) makes its reverse
 * control a throttle that selects R and leaves it for the speed's gear once
 * the throttle is pressed; the clutch mode's MT runs the manual box's
 * (car_shift_manual, and the throttle as TRANS_MANUAL's:
 * pc_clutch_trans_mode) - the reverse control shifts into R, the shift up
 * from R is 1st, the throttle drives it.  A
 * gear taken with the clutch out (p >= 85 %) goes in at once; taken with it
 * in - a mis-shift - the box grinds: no drive for PC_CLUTCH_GRIND_TIME,
 * then the gear goes in as the pedal is (fully in: a hard snap); the clutch
 * pressed past 85 % during the grind ends it.  Neutral needs no clutch.
 *
 * The frames (replays run the cars again from them): RaceInput::read's live
 * frame (pc_clutch_live_input) puts the clutch in CarInput.unk_3 (0 none, 1
 * + p x 254: recorded since this, T318's CHANGED_CLUTCH) and an H-Shifter's
 * gate in CarInput.shift as PC_CLUTCH_GATE_CODE + the gate; the race's
 * clutch mode and H-shifters are its flags (pc_shift.h, the saved race's
 * mark).  A gate code is never a shift: car_process_input drops it in any
 * race.
 *
 * The log: "PC: clutch: ..." - a player's car's first clutch frame, each
 * mis-shift (at most 200 a run). */
#ifndef GAME_APP_PC_CLUTCH_H
#define GAME_APP_PC_CLUTCH_H

#include "types.h"

class RaceCarState;
class CarInput;
class CarCtrlOut;

/* The pedal's travel from which the clutch is out (user: "85% travel counts
 * as fully disengaged"). */
#define PC_CLUTCH_DISENGAGED 0.85f
/* The most the clutch passes, all the way in: this many times the engine's
 * peak torque (a road car's clutch holds 1.2 - 1.5, a racing one 2 - 3). */
#define PC_CLUTCH_CAPACITY 2.0f
/* A mis-shift's grind: the seconds without drive before the gear goes in. */
#define PC_CLUTCH_GRIND_TIME 0.50f
/* CarInput.shift of an H-Shifter's gate: this + 0 (neutral) .. 9, +
 * PC_CLUTCH_GATE_REVERSE the reverse. */
#define PC_CLUTCH_GATE_CODE 0x40
#define PC_CLUTCH_GATE_REVERSE 10

/* RaceInput::read (T413), a live frame after the pad and the wheel: player
 * `player''s clutch and gate into in, when the race being driven runs the
 * clutch mode (wheel: the frame is the wheel's; the other device's clutch
 * counts too). */
void pc_clutch_live_input(int player, CarInput *in, int wheel);
/* PC (2026-11-23, user: "H-Shifter is turned off Alert: If H-shifter inputs
 * are being pressed by the user, popup alerting the user the H-shifter mode
 * is off"): RacePS2Base::virtual_41 arms it as a race is driven (on 1) and
 * drops it after (0); armed, pc_clutch_live_input shows each player's notice
 * once, at the first frame one of their H-shifter's bindings is held while
 * the race does not read it - the gear shift behavior not Realistic + Clutch,
 * H-shifter off, or no clutch mapped (the gates go with the clutch).  8 s,
 * and a log line. */
void pc_hshifter_alert_arm(int on);
/* car_process_input (T342): the frame's clutch and gate for car (a gate
 * code dropped from out->shift in any race). */
void pc_clutch_input(RaceCarState *car, const CarInput *in, CarCtrlOut *out);
/* car_process_input (T342), its throttle: the transmission mode it runs -
 * TRANS_MANUAL for the clutch mode's car in GT3's MT (TRANS_SEMI_AUTO,
 * whose reverse control is a throttle that means R), else transMode. */
int pc_clutch_trans_mode(RaceCarState *car, const CarInput *in, int transMode);
/* car_clutch_from_input (T342): true when car's clutch is its player's
 * (DynCarState.clutch = how far it grips). */
bool pc_clutch_from_input(RaceCarState *car);
/* car_clutch_state (T296): true when the clutch mode set the clutch's
 * state (free, slipping, locked) and the slip's records. */
bool pc_clutch_state(RaceCarState *car);
/* car_drive_torque (T344), slipping: true with the clutch's torque, signed
 * (positive: the engine drives the wheels). */
bool pc_clutch_drive(RaceCarState *car, f32 engineTq, f32 *drive);
/* car_engine_speed (T296), slipping: PC_CLUTCH_GT3 (not the clutch mode's
 * car), PC_CLUTCH_SLIP (it locks where the engine and the wheels meet),
 * PC_CLUTCH_LOCK (the step's drive was within what it holds: it locks now)
 * or PC_CLUTCH_HOLD (not below the idle's wheel speed); the engine kept at
 * its idle at least, GT3's frees of a slipping clutch not taken, nor its
 * lock's 12 steps before the next shift (shiftTimer: a shift button's). */
enum { PC_CLUTCH_GT3 = 0, PC_CLUTCH_SLIP = 1, PC_CLUTCH_HOLD = 2, PC_CLUTCH_LOCK = 3 };
int pc_clutch_engine(RaceCarState *car, f32 wheelSpeed);
/* car_gear_select (T296): true when the clutch mode chose the gear. */
bool pc_clutch_gear_select(RaceCarState *car);
/* DynCar_resetState: car's clutch over. */
void pc_clutch_reset(RaceCarState *car);

#endif /* GAME_APP_PC_CLUTCH_H */
