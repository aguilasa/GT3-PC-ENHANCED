/* car_input_internal.h - unit u079 of the GT3 A-Spec US main executable (SCUS-97102),
 * 0x001E1B28..0x001E4E78.
 *
 * Translation units (link order):
 *   T292  the drivetrain block of a CarSpec (GearCalc: the gear x final
 *         ratios over the driven tyre radius, the shift points, the
 *         differentials, the inertias) and the gear chart of the setting
 *         screen (GearChartData)
 *   T293  the tyre dimensions and the drivetrain layout of a CarSpec
 *   T294  the automatic gear ratios (the "auto" gear setting) and the
 *         adjustable range of every gear
 *   T356  the brake assist: the inner wheels braked against an under- /
 *         oversteer, a side's brakes released (TcsParams), the per-wheel ABS
 *   T357  the traction control: the throttle cut from the driven wheels' slip
 *   T358  the computer driver's input of a car (AIDriver, T361..T367)
 *   T340  the slip estimate (under / oversteer) and the driving aid dispatch
 *   T341  the control mode requests (DynCarState.ctrlMode)
 *   T342  the driver input processing: the steering (a speed-dependent lock,
 *         the steering speed), the throttle / brake / handbrake ramps or
 *         analog curves, the clutch, the input tuning constants
 *
 * The classes and functions are declared in the canonical headers: the own
 * ones (T292, T294, T340, T341, T342, T356) and those of the units they use.
 * The car is RaceCarState (T302; u079's RaceCarDynamics is a typedef of it):
 * +0x10 its CarDynamics (CarParams), +0xEC8 its computer driver (AIDriver;
 * the first pass's "Transmission"), +0xF84 its run state (DynCarState,
 * CarDynState).  This header keeps what is local to the unit: the functions
 * no other unit calls, the globals only this unit reads, named constants.
 *
 * Data pass: none of the headers below emits a type name or a literal
 * (tools/hdrnames.py); T154_race_display.h (the race display classes' names
 * and an inline "") is not included.  The files that saw UnitArena<64>
 * (T356, T358, T341, T342: "bad_alloc" and "t9UnitArena1Ui64" each) include
 * <new> and T153_race_course.h (emitting) themselves, before this header.
 */
#ifndef U079_H
#define U079_H

#include "types.h"
#include <stddef.h>
#include "game/enhanced.h"                      /* ENH_PHYSICS_* (the enhanced engine) */

/* the own translation units */
#include "game/car/physics/T292_gear_calc.h"            /* GearCalc, EngineCurve (= CarEngine), gear_calc, gear_chart_of_spec / gear_chart_with_sheet */
#include "game/car/physics/T294_gear_auto.h"            /* gear_auto_ratios, gear_auto_ranges */
#include "game/car/physics/T356_brake_assist.h"         /* CarWheelState (= DynWheel), TcsParams */
#include "game/car/physics/T340_assist_control.h"       /* car_assists */
#include "game/car/physics/T341_gear_mode.h"            /* car_mode_driver .. car_mode_still */
#include "game/car/physics/T342_car_input.h"            /* CarInput, CarCtrlOut, input_tuning_init, car_set_steer_assist, car_input_update */

/* the classes */
#include "game/data/T482_car_equipments.h"       /* CarSpec */
#include "game/car/physics/T280_car_condition.h"        /* CarCondition */
#include "game/menu/framework/T235_gear_chart_data.h"      /* GearChartData (a plain record: not T235_color_box.h, whose
                                               menu classes' type names no original u079 file has) */
#include "game/car/physics/T291_engine.h"               /* CarEngine, EngineSpecSheet, engine_setup, engine_spec_sheet */
#include "game/car/physics/T296_car_drivetrain.h"       /* TyreData, CarDynState (= DynCarState), RaceCarDynamics (= RaceCarState) */
#include "game/car/physics/T298_suspension_body.h"      /* CarParams (= CarDynamics) */
#include "game/car/physics/T302_car_state.h"            /* RaceCarState, car_set_wheel_brakes, car_set_brake */
#include "game/race/dynamics/T305_dynamics_conductor.h"   /* DynamicsConductor */
#include "game/race/dynamics/T361_drive_params.h"         /* AIDriver (Transmission) */
#include "game/race/core/T413_race_input.h"           /* RaceInput::read */

/* the functions and globals of other units */
#include "game/car/physics/T284_car_param.h"            /* Curve, CarDynamics_getTyre */
#include "game/common/T322_curve_linear.h"         /* curveLookupLinear */
#include "game/common/T354_response_curve.h"       /* ResponseCurve, responseInit, responseEval */
#include "game/car/physics/T336_steer_force.h"          /* steer_force_init */
#include "game/race/ai/T363_ai_gear.h"              /* ai_select_gear, ai_update_gear */
#include "game/race/ai/T366_ai_drive.h"             /* ai_idle, ai_apply_outputs, ai_drive */
#include "game/race/ai/T367_ai_line.h"              /* ai_change_line, ai_rev_engine */
#include "game/car/physics/T325_misc.h"                 /* g_002D28B0 (DynParams, T283) */
#include "game/car/physics/T339_misc.h"                 /* g_002D4628 (InputTuning) */
#include "engine/media/G002_mpeg_frameq.h"          /* sinf */

/* ======================================================================== */
/* Named constants                                                          */
/* ======================================================================== */

/* GearCalc.driveType: DRIVE_* (T292).  CarSpec.layout: LAYOUT_FR ..
 * LAYOUT_RR (T482). */

/* CarInput.flags: the analog inputs. */
enum {
    INPUT_ANALOG_STEER     = 0x01,
    INPUT_ANALOG_THROTTLE  = 0x02,
    INPUT_ANALOG_BRAKE     = 0x04,
    INPUT_ANALOG_THROTTLE2 = 0x08,
    INPUT_ANALOG_HANDBRAKE = 0x10
};

/* CarInput.device: the steering source. */
enum {
    INPUT_DEV_DIGITAL  = 0,
    INPUT_DEV_STICK    = 2,                 /* the analog stick */
    INPUT_DEV_PRESSURE = 3,                 /* the pressure buttons: steered like the digital pad */
    INPUT_DEV_WHEEL    = 4,                 /* a steering wheel */
    INPUT_DEV_DFP      = 6,                 /* the enhanced engine: the Driving Force Pro (Lupo 2; RaceInput::readWheel's
                                               STEER_WHEEL_DFP): the full lock at once, its input predicted
                                               (physics.md 19) */
    INPUT_DEV_WHEEL_DIRECT = 7              /* PC (2026-11-20): a wheel with its steering filter off ([WheelN]
                                               SteeringFilter=off; RaceInput::readWheel's STEER_WHEEL_DIRECT): the
                                               full lock straight from the input - no speed curve, no lag, no lock
                                               following the slide, no prediction; the wheel's pedal curves.  Recorded
                                               with the input: a replay or ghost steers as the race did */
};

#define INPUT_FULL 0x1000                   /* an analog input at full */
#define INPUT_ONE  (1.0f / 4096.0f)         /* 0x39800000: 1 / INPUT_FULL */

/* CarCtrlOut.steerKind. */
enum {
    STEER_KIND_WHEEL   = 0,
    STEER_KIND_DIGITAL = 1,
    STEER_KIND_ANALOG  = 2
};

/* CarCtrlOut.throttleKey (digital throttle). */
enum {
    THROTTLE_KEY_NONE  = 0,
    THROTTLE_KEY_ONE   = 1,                 /* the throttle key held */
    THROTTLE_KEY_BOTH  = 2                  /* the second throttle (the reverse) too */
};

/* DynCarState.driver / ctrlMode / transMode / clutchState: DRIVER_*,
 * CTRL_*, TRANS_*, CLUTCH_* (T288). */

/* The enemy lines ai_change_line moves to (T368). */
enum {
    LINE_PIT_ENTRY = 3,                     /* the way into the pit lane */
    LINE_PIT_LANE  = 4,
    LINE_RACING    = 6,
    LINE_7         = 7                      /* (? the pad car's own line; falls back to 6) */
};

/* The ramps per frame, g_002D4930[] (InputTuning.rampTime[]). */
enum {
    RAMP_THROTTLE_UP   = 0,
    RAMP_THROTTLE_DOWN = 1,
    RAMP_BRAKE_UP      = 2,
    RAMP_BRAKE_DOWN    = 3,
    RAMP_HANDBRAKE_UP  = 4,
    RAMP_HANDBRAKE_DOWN = 5
};

#define DEG2RAD 0.017453292f                /* 0x3C8EFA35 */

/* ======================================================================== */
/* Globals of this unit                                                     */
/* ======================================================================== */
extern const char g_00352968[8];    /* 0x00352968 gp-0x5088 (T292 .sdata) "frnmhvyt" (not terminated): the differential letters */
extern u8 g_002D43C8[3][6];         /* 0x002D43C8 (T294 .data) the auto ratio steps of 5, 6, 7 gears, each 200 / the
                                       byte: [1] / [0] the step at level 150 / 400, [3] / [2] its growth, [5] / [4]
                                       the 1st gear's extra step */
extern f32 g_002D4930[6];           /* 0x002D4930 (T342 .data) per frame, RAMP_*: 100 / (InputTuning.rampTime x 60), <= 1 */
extern u8 g_002D4948[16];           /* 0x002D4948 (T342 .data) per surface type (DynWheel.surface): its steering class
                                       (? 1: loose ground; car_process_input) */
/* (T342's .bss 0x0049D060..0x0049D0AC, the wheel controller's curves and the
 * steering speeds, are file statics of T342.) */
extern f32 g_00352970;             /* 0x00352970 gp-0x5080 (T342 .sdata) the digital steering's spring (100) */
extern f32 g_00352974;              /* 0x00352974 gp-0x507C its damping (5) */
extern f32 g_00352978;              /* 0x00352978 gp-0x5078 the centring spring (50) */
extern f32 g_0035297C;              /* 0x0035297C gp-0x5074 how fast the wheel's lock follows, rad/s (pi / 2) */
extern f32 g_00352980;              /* 0x00352980 gp-0x5070 the wheel's minimum lock, steering assist 1 / 3 (8 deg) */
extern f32 g_00352984;              /* 0x00352984 gp-0x506C the same, steering class 1 (15 deg) */
extern f32 g_00352988;              /* 0x00352988 gp-0x5068 the minimum lock of steering assist 2 (12 deg; no init) */
extern f32 g_0035298C;              /* 0x0035298C gp-0x5064 the same, steering class 1 (20 deg) */
extern f32 g_00352990;              /* 0x00352990 gp-0x5060 InputTuning.analogPedalMin x 0.01: a pressed analog
                                       pedal x gives x + it (1 - x) */

/* ======================================================================== */
/* This unit's functions that no canonical header declares                  */
/* ======================================================================== */
/* T292 */
s8   diff_code_to_index(s8 c);                                          /* 0x001E1B28 */
void diff_codes_to_index(CarSpec *spec);                                /* 0x001E1B80 */
void diff_codes_to_letters(CarSpec *spec);                              /* 0x001E1BC8 */
int  gear_auto_apply(CarSpec *spec, int unused);                        /* 0x001E1C10 */
void gear_chart_fill(GearCalc *g, EngineCurve *e, GearChartData *out);  /* 0x001E2450 */
/* T293 */
void tyre_dimensions(CarSpec *spec, int axle, f32 *radius, f32 *width, f32 *sidewall); /* 0x001E2670 */
u8   drivetrain_layout(CarSpec *spec, u8 *type, u8 *axle, f32 *split, f32 *torque);   /* 0x001E2700 */
/* T294 */
void gear_ranges(int n, s16 *ratios, s16 *lo, s16 *hi);                 /* 0x001E2B30 */
/* T356 */
void wheel_add_brake(CarWheelState *w, f32 v);                          /* 0x001E2CC8 */
int  tcs_brake_or_abs(TcsParams *t);                                    /* 0x001E2CF8 */
void tcs_brakes(CarDynState *st, CarParams *p, TcsParams *t, f32 *slipLimit, int enable); /* 0x001E2D08 */
void car_brake_aids(RaceCarDynamics *car, TcsParams *t, int enable);    /* 0x001E3068 */
/* T357 */
f32  fade_linear(f32 x, f32 gain, f32 start);                           /* 0x001E32D8 */
int  tcs_throttle_on(TcsParams *t);                                     /* 0x001E3338 */
void car_traction_control(RaceCarDynamics *car, TcsParams *t);          /* 0x001E3360 */
/* T358 */
void car_input_ai(RaceCarDynamics *car, CarInput *in);                  /* 0x001E35D8 */
/* T340 */
void car_slip_estimate(RaceCarDynamics *car);                           /* 0x001E3708 */
void car_assists_own(RaceCarDynamics *car);                             /* 0x001E3A68 */
void car_assists_ai(RaceCarDynamics *car);                              /* 0x001E3AD0 */
/* T341 */
/* (car_mode_stop_roll, 0x001E3DF0: T341_gear_mode.h, for the Pod race) */
/* T342 */
void car_clutch_from_input(RaceCarDynamics *car, CarInput *in, CarCtrlOut *out, int transMode); /* 0x001E4180 */
void car_steering_display(CarDynState *st, CarInput *in);               /* 0x001E4290 */
void approach(f32 *v, f32 target, f32 step);                            /* 0x001E4320 */
void car_steer_toward(RaceCarDynamics *car, f32 target, f32 speed, f32 speedBack); /* 0x001E4368 */
s8   car_get_steer_assist(RaceCarDynamics *car);                        /* 0x001E4470 */
void car_process_input(RaceCarDynamics *car, CarInput *in, CarCtrlOut *out); /* 0x001E4478 */
void car_input_pad(RaceCarDynamics *car, CarInput *in);                 /* 0x001E4CB0 */
void car_input_ai_apply(RaceCarDynamics *car);                          /* 0x001E4CF0 */
/* T342, the enhanced engine (new, Lupo 2) */
void car_clutch_electric(CarDynState *st);                              /* L2 0x001F9328 (also T358) */
int  car_steer_display(f32 steer);                                      /* L2 0x001F96C0 */

/* ======================================================================== */
/* Inline helpers                                                           */
/* ======================================================================== */
/* The tyre parameter set of wheel i on the wheel's surface (an overload of
 * T284's CarDynamics_getTyre taking the wheel).  Retail keeps the wheel's
 * address in the loops that use it (car_brake_aids, car_slip_estimate,
 * car_traction_control) and reads the surface at +0x24: the wheel came in
 * as an inline argument (a direct st->wheel[i].surface folds to +0x84). */
static inline TyreData *CarDynamics_getTyre(CarParams *p, int i, CarWheelState *w)
{
    return CarDynamics_getTyre(p, i, w->surface);
}

#endif /* U079_H */
