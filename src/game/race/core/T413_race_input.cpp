/* T413_race_input.cpp - RaceInput (0x002104E8..0x00211A00).
 *
 * The input of one player in a race.  emulatePressure (slot 9) turns the
 * pad / wheel buttons into function bits through the controller settings;
 * read() fills the car's CarInput from the pad (by pad type) and the USB
 * wheel, then records it into `rec' (live) or plays the record back
 * (replay).
 *
 * Retail T413's .rodata (0x0033F750..0x0033F967) opens with <new>'s
 * "bad_alloc" (nothing references it: DATA_START), then emulatePressure's
 * jump table, the vtable and the type names 9RaceInput, 14RaceCameraBase,
 * t9UnitArena1Ui64 and the garage's (15GarageWheelList .. Cell<GarageCar>,
 * GarageCar's base list among them): the file saw the GameStatus first,
 * then UnitArena<64> (T153) and RaceCameraBase (T383), RaceInput last (the
 * names come in reverse definition order).  No operator list names: the
 * analog stick curve comes from its own header (T472).
 */
// DATA_START 0x0033F750
#include <new>                                  /* (unused: "bad_alloc") */
#include "game/save/T429_game_status.h"              /* gt2k_game::GameStatus, getGameStatus (the garage names) */
#define GAME_T153_EMITS_TYPEINFO                /* UnitArena<64>'s type name, as retail */
#include "game/race/core/T153_race_course.h"              /* (unused: "t9UnitArena1Ui64") */
#include "game/camera/T383_race_camera_base.h"         /* (unused: "14RaceCameraBase", only pure virtuals) */
#include "game/race/core/race_input_internal.h"
#include "game/race/dynamics/T318_control_record.h"           /* AutomobileControlRecord::Manager / Recorder */
#include "game/car/physics/T342_car_input.h"                /* CarInput */
#include "game/sound/T288_car_sound.h"                /* PadVibration */
#include "game/save/T426_controller_config.h"        /* ControllerConfig */
#include "game/save/T428_game_options.h"             /* GameOptions (forceFeedback1p / 2p, config) */
#include "engine/input/T472_analog_curve.h"             /* g_003416F8, the analog stick curve (PC: not read here) */
#include "game/race/core/T413_race_input.h"               /* RaceInput, FuncBit, g_002DFB18, g_002DFB58, raceinput_* */
#include "game/enhanced.h"                      /* ENH_CAMERA_ZOOM_BUTTON (the cameras' switch) */
#include "game/app/pc_frame.h"                  /* PC: pc_replay_speed_buttons */
#include "game/app/pc_clutch.h"                 /* PC (2026-11-09): the clutch mode's frames */
#include "game/app/pc_ffb.h"                    /* PC (2026-11-20): the force feedback option's notice */
#include "platform/input_map.h"                 /* PC (2026-11-20): the wheels' steering filter and curve */

/* The configured functions 1..8 and their button bits (T413's .data
 * 0x002DFB18, writable in retail: .data, not .rodata). */
FuncBit g_002DFB18[8] = {                       /* 0x002DFB18 */
    { 1, RIB_THROTTLE },
    { 2, RIB_BRAKE },
    { 3, RIB_THROTTLE2 },
    { 4, RIB_HANDBRAKE },
    { 5, RIB_SHIFT_UP },
    { 6, RIB_SHIFT_DOWN },
    { 7, RIB_FUNC7 },
    { 8, 0x20000 }
};

// FUNC 0x002104E8 RaceInput::RaceInput
//   Control::Control, then rec's AutomobileControlRecord::Manager::Manager
//   (if that throws, the landing pad runs Control::~Control).
RaceInput::RaceInput()
    : Control(), rec()
{
    /* implicit: Manager (compiler-generated: bases, members, deleting form) */
    replayMode = REPLAY_LIVE;
    enabled = 1;
    lastReplayMode = REPLAY_LIVE;
    vibration = 0;
    hold = 0;
    port = 0;
    bestLap = 0;
    lap = 0;
    snapshots = 0;
}

// FUNC 0x00210578 RaceInput::RaceInput(int)
//   As RaceInput() (Control, Manager; Control::~Control on a throw), but it
//   leaves lastReplayMode, port and snapshots unset.
RaceInput::RaceInput(int enable)
    : Control(), rec()
{
    /* implicit: Manager (compiler-generated: bases, members, deleting form) */
    enabled = enable;
    replayMode = REPLAY_LIVE;
    vibration = 0;
    hold = 0;
    bestLap = 0;
    lap = 0;
}

// FUNC 0x00210608 RaceInput::~RaceInput
//   rec.~Manager() (0x001F4F20), then Control::~Control.
RaceInput::~RaceInput()
{
    /* implicit: ~Manager, ~Control (compiler-generated: bases, members, deleting form) */
}

// FUNC 0x00210658 RaceInput::setReplayMode
void RaceInput::setReplayMode(int mode)
{
    replayMode = mode;
    rec.playback = (mode != REPLAY_LIVE);
}

// FUNC 0x00210668 RaceInput::resetLap
void RaceInput::resetLap(u32 startTime)
{
    if (lap != 0)
        lap->reset();
}

// FUNC 0x00210690 RaceInput::isLapFinished
int RaceInput::isLapFinished()
{
    if (lap == 0)
        return 0;
    return lap->rec.finished;
}

// FUNC 0x002106B0 RaceInput::recordLap
void RaceInput::recordLap(const CarInput *in)
{
    if (lap != 0)
        lap->rec.record(in);
}

// FUNC 0x002106D8 RaceInput::stopLap
void RaceInput::stopLap()
{
    if (lap != 0)
        lap->rec.stop();
}

// FUNC 0x00210700 RaceInput::keepLap
//   The current lap's record becomes the best one (live input only).
void RaceInput::keepLap()
{
    if (bestLap != 0 && lap != 0 && replayMode == REPLAY_LIVE)
        bestLap->copySlotFrom(lap);
}

// FUNC 0x00210740 RaceInput::read
//   Live (mode 0 or 3): reads the pad and the wheel and records the frame;
//   otherwise plays the next recorded frame unless held.
void RaceInput::read(CarInput *in)
{
    in->clear();
    if (enabled == 0)
        return;
    if (replayMode != REPLAY_LIVE && replayMode != REPLAY_RECORDED) {
        if (hold == 0)
            rec.rec.play(in);
    } else {
        readPad(in);
        readWheel(in);
        /* PC (2026-11-09): the clutch mode's clutch and H-shifter gate of the
         * player, recorded with the frame (game/app/pc_clutch.h) */
        pc_clutch_live_input(port, in, in->device == STEER_WHEEL || in->device == STEER_WHEEL_DFP ||
                                           in->device == STEER_WHEEL_DIRECT);     /* (PC 2026-11-20) */
        rec.rec.record(in);
    }
}

// FUNC 0x002107D8 RaceInput::sendVibration
//   Actuators 0 / 1 (when vibration is on) and the wheel's force (actuator
//   2, when the player's force feedback option is on and a USB wheel is
//   there).  Live input only.
void RaceInput::sendVibration(const PadVibration *v)
{
    if (enabled == 0)
        return;
    if (replayMode != REPLAY_LIVE)
        return;
    if (vibration != 0) {
        int small = v->small;
        int big = v->big;

        /* abs and the 0xFF clamp are dead on the u8 values, kept as the
         * code does them */
        if (small < 0)
            small = -small;
        if (big < 0)
            big = -big;
        if (small >= 0x100)
            small = 0xFF;
        if (big >= 0x100)
            big = 0xFF;
        setActuator(0, small);
        setActuator(1, big);
    }
    /* the player's force feedback option (GameStatus's GameOptions) */
    if ((port == 0) ? getGameStatus()->getGameOptions()->forceFeedback1p
                    : getGameStatus()->getGameOptions()->forceFeedback2p) {
        if (rawUSBType() != USB_WHEEL_NONE)     /* lbu +0x14, dsrl 5 */
            setActuator(2, v->steer);
    }
}

// FUNC 0x002108F0 raceinput_padButtons
//   The function bits of the pad: d-pad left / right -> 0x100 / 0x200, each
//   configured function's button (keyButton) -> its bit; the raw buttons
//   exactly 0x8013 add 0x20000000.
u32 raceinput_padButtons(ControllerConfig *cfg, int mode, int port, u32 raw)
{
    u32 bits = 0;
    const FuncBit *entry = g_002DFB18;
    int n;

    if (raw & PADRAW_LEFT)
        bits |= RIB_STEER_LEFT;
    if (raw & PADRAW_RIGHT)
        bits |= RIB_STEER_RIGHT;
    for (n = 0; n < 8; n++, entry++) {
        if (raw & cfg->keyButton(mode, port, entry->func))
            bits |= entry->bit;
    }
    if (raw == PADRAW_COMBO_8013)
        bits |= RIB_COMBO_8013;
    return bits;
}

// FUNC 0x002109C0 raceinput_wheelButtons
//   Each configured function's wheel button (-1: none) -> its bit.
u32 raceinput_wheelButtons(ControllerConfig *cfg, int mode, int port, u32 raw)
{
    u32 bits = 0;
    const FuncBit *entry = g_002DFB18;
    int n;

    for (n = 0; n < 8; n++, entry++) {
        int mask = cfg->keyButton(mode, port, entry->func);

        if (mask != -1 && (raw & mask))
            bits |= entry->bit;
    }
    return bits;
}

// FUNC 0x00210A78 RaceInput::getControllerConfig
//   The controller settings (GameStatus +0x30570).
ControllerConfig *RaceInput::getControllerConfig()
{
    return &getGameStatus()->options.config;
}

// FUNC 0x00210AA0 RaceInput::emulatePressure
//   Control::emulatePressure, then the buttons word: the low byte of
//   getButtons(), the pad's function bits (config mode 0 digital, 1
//   analog), the wheel's (mode 2 / 3 by wheel type, fixed bits for the 0x6F8
//   wheel), and in replay modes 1..3 the replay bits from the raw buttons.
//   A change of replay mode clears curButtons.mask for the frame.  PS type 0
//   keeps raw[0] as the wheel's buttons; every other type takes raw[1].
//   (u095 matching: the replay modes as a switch, retail's two signed
//   tests; raw[1] read in both cases that take it, the second load is
//   merged later but decides `this' / `mode' register order.)
void RaceInput::emulatePressure()
{
    int configMode = CONFIG_DIGITAL;
    ControllerConfig *cfg;
    int player;
    u32 raw;
    u32 out;
    int mode;
    int psType;
    int usbType;

    Control::emulatePressure();
    cfg = getControllerConfig();
    player = port;
    raw = curButtons.raw[0];
    out = getButtons() & 0xFF;
    mode = replayMode;
    psType = rawPSType();
    usbType = raceinput_usbType(this);
    if (lastReplayMode != mode) {
        lastReplayMode = mode;
        curButtons.mask = 0;
    }

    switch (psType) {                           /* table 0x0033F760 (32 cases) */
    case PS_PAD_NONE:
        break;
    case PS_PAD_ANALOG:
    case PS_PAD_DUALSHOCK:
    case PS_PAD_DUALSHOCK2:
        configMode = CONFIG_ANALOG;
        /* fallthrough */
    default:
        out |= raceinput_padButtons(cfg, configMode, player, raw);
        switch (mode) {
        case 1:
        case 2:
        case 3:
            if (raw & PADRAW_UP)
                out |= RIB_REPLAY_UP;
            if (raw & PADRAW_DOWN)
                out |= RIB_REPLAY_DOWN;
            if (raw & PADRAW_LEFT)
                out |= RIB_REPLAY_LEFT;
            if (raw & PADRAW_RIGHT)
                out |= RIB_REPLAY_RIGHT;
            if (raw & PADRAW_CROSS) {
                out |= RIB_REPLAY_CROSS;
                out |= RIB_REPLAY_CROSS2;
            }
            if (raw & PADRAW_CIRCLE)
                out |= RIB_REPLAY_CIRCLE;
            if (raw & PADRAW_L1)
                out |= RIB_REPLAY_L1;
            if (raw & PADRAW_R1)
                out |= RIB_REPLAY_R1;
#if ENH_CAMERA_ZOOM_BUTTON
            if (raw & PADRAW_TRIANGLE)          /* Lupo 2: the zoom (L2 0x0021D1CC) */
                out |= RIB_REPLAY_ZOOM;
#endif
            /* PC: L2 / R2 step the replay's speed (game/app/pc_frame.h): here
             * for the pad's own bits - a lone wheel's are in raw[0] too, where
             * 1 and 2 are its replay buttons - and not with L1 or R1 held,
             * all four being the logo's toggle (T132) */
            if ((raw & (PADRAW_L1 | PADRAW_R1)) == 0)
                pc_replay_speed_buttons(player, raw & PADRAW_L2, raw & PADRAW_R2);
            break;
        }
        raw = curButtons.raw[1];
        break;
    case PS_PAD_TYPE_31:
        raw = curButtons.raw[1];
        break;
    }

    switch (usbType) {
    case USB_WHEEL_GTFORCE:
        out |= raceinput_wheelButtons(cfg, CONFIG_WHEEL_GTFORCE, player, raw);
        switch (mode) {
        case 1:
        case 2:
        case 3:
            if (raw & 0x1)
                out |= RIB_REPLAY_UP;
            if (raw & 0x8)
                out |= RIB_REPLAY_DOWN;
            if (raw & 0x2)
                out |= RIB_REPLAY_CROSS;
#if ENH_CAMERA_ZOOM_BUTTON
            if (raw & 0x10)                     /* Lupo 2: the zoom (L2 0x0021D2AC) */
                out |= RIB_REPLAY_ZOOM;
#endif
            break;
        }
        break;
    case USB_WHEEL_C294:
    case USB_WHEEL_DFP:                         /* the enhanced engine: the DFP has the C294's buttons
                                                   (backend.md B1; Lupo 2 L2 0x0021D248) */
        out |= raceinput_wheelButtons(cfg, CONFIG_WHEEL_C294, player, raw);
        switch (mode) {
        case 1:
        case 2:
        case 3:
            if (raw & 0x1)
                out |= RIB_REPLAY_UP;
            if (raw & 0x8)
                out |= RIB_REPLAY_DOWN;
            if (raw & 0x2)
                out |= RIB_REPLAY_CROSS;
            break;
        }
        break;
    case USB_WHEEL_6F8:                          /* fixed buttons (usb6F8Buttons, T479) */
        if (raw & 0x5)
            out |= RIB_SHIFT_DOWN;
        if (raw & 0x28)
            out |= RIB_SHIFT_UP;
        if (raw & 0x3C0)
            out |= RIB_HANDBRAKE;
        if (raw & 0x3C00)
            out |= RIB_THROTTLE2;
        if (raw & 0x8000)
            out |= RIB_FUNC7;
        break;
    }
    curButtons.buttons = out;
}

// FUNC 0x00210D68 raceinput_digitalSteer
//   d-pad steering: +2 left, -2 right (car_input_update ramps it).
void raceinput_digitalSteer(CarInput *in, u32 bits)
{
    in->device = STEER_DIGITAL;
    in->flags &= ~CARINPUT_ANALOG_STEER;
    in->steer = (bits & RIB_STEER_LEFT) ? 2 : ((bits & RIB_STEER_RIGHT) ? -2 : 0);
}

/* A pressure value (the low byte of a channel, 0..0xFF) as 0..0x100, full
 * at `scale' %.  Inline in the original: readPressureSteer and
 * raceinput_analogValue expand it (retail's division-by-zero check on the
 * parameter's register at the top of readPressureSteer, and the second
 * clamp into a fresh register in both).  (? name)
 * PC: no sensitivity scaling (docs/ROADMAP.md, "Button sensitivity
 * scaling"): the value runs linearly from 0 to 0x100 over the button's whole
 * travel, `scale' unused.  The original doubled it, full at half the travel,
 * then made `scale' % of that full:
 *     v = min((value & 0xFF) << 1, 0x100);  v = min(v * 100 / scale, 0x100);
 * so the throttle was full at 17.5 % of the travel, the brake at 32.5 %, the
 * handbrake at 10 %, the d-pad's steering at 30 %. */
static inline int raceinput_pressure(int value, int scale)
{
    (void)scale;
    return ((value & 0xFF) * 0x100 + 0x7F) / 0xFF;
}

/* PC: a stick byte (0x80 the centre) as 0x100 (0x00, full left / up) ..
 * -0x100 (0xFF, full right / down), linear on each side: the race reads its
 * sticks without the curve g_003416F8 (T472), whose dead zone (0x5B..0xA4)
 * and clipped ends (0x00..0x0F, 0xF0..0xFF) cut the stick's range ("Button
 * sensitivity scaling" above; the menus keep the curve for their
 * directions).  Centring a worn stick is the launcher's calibration, later. */
static inline int raceinput_stick(int value)
{
    int v = value & 0xFF;

    if (v <= 0x80)
        return (0x80 - v) * 2;
    return -(((v - 0x80) * 0x100 + 0x3F) / 0x7F);
}

// FUNC 0x00210DA8 RaceInput::readPressureSteer
//   Steering from the pressures of d-pad left (channel 5) and right
//   (channel 4): full at 60 % pressure (PC: at full pressure,
//   raceinput_pressure).
void RaceInput::readPressureSteer(CarInput *in)
{
    int left = raceinput_pressure(channels[CHAN_PRESS_LEFT], 60);
    int right = raceinput_pressure(channels[CHAN_PRESS_RIGHT], 60);
    int larger;
    int steer;

    if (left == 0 && right == 0) {
        in->steer = 0;
        in->device = STEER_DIGITAL;
        in->flags &= ~CARINPUT_ANALOG_STEER;
        return;
    }
    in->flags |= CARINPUT_ANALOG_STEER;
    in->device = STEER_PRESSURE;
    steer = (left - right) << 4;
    in->steer = steer;
    larger = (left < right) ? right : left;
    in->steerMag = larger << 4;
}

// FUNC 0x00210E68 RaceInput::readAnalogSteer
//   Function 0 on an analog stick (keyAxis), through the stick curve,
//   unless the steer is already set.
//   PC: the stick as it is (raceinput_stick); the original's
//   (0x100 - g_003416F8[x] * 2) << 4 went through the curve.
void RaceInput::readAnalogSteer(CarInput *in, ControllerConfig *cfg, int mode, int port)
{
    int axis;

    if (in->steer != 0)
        return;
    axis = cfg->keyAxis(mode, port, KEY_FUNC_STEER);
    if (axis >= 0) {
        in->flags |= CARINPUT_ANALOG_STEER;
        in->device = STEER_ANALOG;
        in->steer = raceinput_stick(channels[axis]) << 4;
    }
}

// FUNC 0x00210EF8 raceinput_digitalButtons
//   The function bits as digital values (1, the analog flag cleared); shift
//   down wins over up.
void raceinput_digitalButtons(CarInput *in, u32 bits)
{
    if (bits & RIB_THROTTLE) {
        in->throttle = 1;
        in->flags &= ~CARINPUT_ANALOG_THROTTLE;
    }
    if (bits & RIB_BRAKE) {
        in->brake = 1;
        in->flags &= ~CARINPUT_ANALOG_BRAKE;
    }
    if (bits & RIB_HANDBRAKE) {
        in->handbrake = 1;
        in->flags &= ~CARINPUT_ANALOG_HANDBRAKE;
    }
    if (bits & RIB_THROTTLE2) {
        in->throttle2 = 1;
        in->flags &= ~CARINPUT_ANALOG_THROTTLE2;
    }
    in->shift = (bits & RIB_SHIFT_DOWN) ? -1 : ((bits & RIB_SHIFT_UP) ? 1 : 0);
}

// FUNC 0x00210F90 raceinput_analogValue
//   Channels 0-3 (sticks) through the curve: the half selected by idx bits
//   0x100 / 0x200 (0..0xFE), else the curve value itself; other channels
//   (pressures) scaled so that `scale' % gives 0x100.  The channel is a
//   switch with the cases 0..3 (retail's two tests, slti 4 / bltz: an `&&'
//   folds into one unsigned range test); the result is `value' itself.
//   PC: the sticks without the curve, each half 0..0x100 (raceinput_stick)
//   and a whole axis 0..0x100 too; the pressures over their whole travel
//   (raceinput_pressure).  The original: x = g_003416F8[value], the lower
//   half (0x80 - x) * 2, the upper (x - 0x80) * 2 (0..0xFE), else x.
int raceinput_analogValue(int idx, int value, int scale)
{
    switch (idx & 0xFF) {
    case 0:                                     /* the sticks: the curve */
    case 1:
    case 2:
    case 3:
        switch (idx & 0x300) {
        case 0x100:                             /* the lower half */
            value = raceinput_stick(value);
            if (value < 0)
                value = 0;
            break;
        case 0x200:                             /* the upper half */
            value = -raceinput_stick(value);
            if (value < 0)
                value = 0;
            break;
        default:                                /* the whole axis */
            value = raceinput_pressure(value, 100);
            break;
        }
        break;
    default:                                    /* a pressure: `scale' % is full */
        value = raceinput_pressure(value, scale);
        break;
    }
    return value;
}

// FUNC 0x00211050 RaceInput::readAnalogButtons
//   Functions 1, 2, 4, 3 on analog inputs (keyAxis, 100 % scale).  Each
//   channel value is read before the flags byte is stored (retail's lw
//   before the sb: a char store would keep a later int load behind it).
void RaceInput::readAnalogButtons(CarInput *in, ControllerConfig *cfg, int mode, int port)
{
    int axis;

    axis = cfg->keyAxis(mode, port, KEY_FUNC_THROTTLE);
    if (axis >= 0) {
        int value = channels[axis & 0xFF];

        in->flags |= CARINPUT_ANALOG_THROTTLE;
        in->throttle = raceinput_analogValue(axis, value, 100) << 4;
    }
    axis = cfg->keyAxis(mode, port, KEY_FUNC_BRAKE);
    if (axis >= 0) {
        int value = channels[axis & 0xFF];

        in->flags |= CARINPUT_ANALOG_BRAKE;
        in->brake = raceinput_analogValue(axis, value, 100) << 4;
    }
    axis = cfg->keyAxis(mode, port, KEY_FUNC_HANDBRAKE);
    if (axis >= 0) {
        int value = channels[axis & 0xFF];

        in->flags |= CARINPUT_ANALOG_HANDBRAKE;
        in->handbrake = raceinput_analogValue(axis, value, 100) << 4;
    }
    axis = cfg->keyAxis(mode, port, KEY_FUNC_THROTTLE2);
    if (axis >= 0) {
        int value = channels[axis & 0xFF];

        in->flags |= CARINPUT_ANALOG_THROTTLE2;
        in->throttle2 = raceinput_analogValue(axis, value, 100) << 4;
    }
}

// FUNC 0x002111C0 RaceInput::readPressureButtons
//   As readAnalogButtons with the pressure inputs (keyPressure): full at
//   35 % (throttle), 65 % (brake), 20 % (handbrake) and 35 % (throttle2).
//   PC: each full at full pressure (raceinput_pressure ignores the scales).
void RaceInput::readPressureButtons(CarInput *in, ControllerConfig *cfg, int mode, int port)
{
    int ch;

    ch = cfg->keyPressure(mode, port, KEY_FUNC_THROTTLE);
    if (ch >= 0) {
        int value = channels[ch & 0xFF];

        in->flags |= CARINPUT_ANALOG_THROTTLE;
        in->throttle = raceinput_analogValue(ch, value, 35) << 4;
    }
    ch = cfg->keyPressure(mode, port, KEY_FUNC_BRAKE);
    if (ch >= 0) {
        int value = channels[ch & 0xFF];

        in->flags |= CARINPUT_ANALOG_BRAKE;
        in->brake = raceinput_analogValue(ch, value, 65) << 4;
    }
    ch = cfg->keyPressure(mode, port, KEY_FUNC_HANDBRAKE);
    if (ch >= 0) {
        int value = channels[ch & 0xFF];

        in->flags |= CARINPUT_ANALOG_HANDBRAKE;
        in->handbrake = raceinput_analogValue(ch, value, 20) << 4;
    }
    ch = cfg->keyPressure(mode, port, KEY_FUNC_THROTTLE2);
    if (ch >= 0) {
        int value = channels[ch & 0xFF];

        in->flags |= CARINPUT_ANALOG_THROTTLE2;
        in->throttle2 = raceinput_analogValue(ch, value, 35) << 4;
    }
}

// FUNC 0x00211330 RaceInput::isAnalogPressed
//   Function func's analog input (keyAxis) at half or more.
int RaceInput::isAnalogPressed(ControllerConfig *cfg, int mode, int port, int func)
{
    int axis = cfg->keyAxis(mode, port, func);

    if (axis >= 0)
        return (raceinput_analogValue(axis, channels[axis & 0xFF], 100) << 4) >= 0x800;
    return 0;
}

// FUNC 0x00211398 RaceInput::readAnalogShift
//   Functions 5 (up) and 6 (down) on analog inputs; only a non-zero result
//   overrides the digital shift.
void RaceInput::readAnalogShift(CarInput *in, ControllerConfig *cfg, int mode, int port)
{
    int shift = 0;

    if (isAnalogPressed(cfg, mode, port, KEY_FUNC_SHIFT_UP))
        shift = 1;
    if (isAnalogPressed(cfg, mode, port, KEY_FUNC_SHIFT_DOWN))
        shift--;
    if (shift != 0)
        in->shift = shift;
}

// FUNC 0x00211430 RaceInput::readPad
//   By PS type: 4 digital, 5 / 7 analog, 8 pressure-sensitive (config mode
//   1 for all three non-digital kinds); other types read nothing.
void RaceInput::readPad(CarInput *in)
{
    ControllerConfig *cfg = getControllerConfig();
    int player = port;
    u32 bits = getButtons();

    switch (rawPSType()) {
    case PS_PAD_DIGITAL:
        raceinput_digitalButtons(in, bits);
        raceinput_digitalSteer(in, bits);
        break;
    case PS_PAD_ANALOG:
    case PS_PAD_DUALSHOCK:
        raceinput_digitalButtons(in, bits);
        raceinput_digitalSteer(in, bits);
        readAnalogSteer(in, cfg, CONFIG_ANALOG, player);
        readAnalogButtons(in, cfg, CONFIG_ANALOG, player);
        readAnalogShift(in, cfg, CONFIG_ANALOG, player);
        break;
    case PS_PAD_DUALSHOCK2:
        raceinput_digitalButtons(in, bits);
        readPressureSteer(in);
        readAnalogSteer(in, cfg, CONFIG_ANALOG, player);
        readPressureButtons(in, cfg, CONFIG_ANALOG, player);
        readAnalogShift(in, cfg, CONFIG_ANALOG, player);
        break;
    }
}

// FUNC 0x002115D8 raceinput_wheelPedal
//   A wheel pedal (0xFF released): (0xFF - value - deadZone) * 2, at most
//   0x100 (negative inside the dead zone).
//   PC: linear over the pedal's whole travel, 0 .. 0x100, `deadZone' unused
//   (as raceinput_pressure): the wheel's pedal dead zones are the player's
//   (gt3pc.ini [Wheel1] GasDeadzone / BrakeDeadzone, platform/wheel.h).  The
//   original's started at 29 % (gas) / 31 % (brake) of the travel and was
//   full at 79 % / 81 %:
//       v = 0xFF - value;  v = min((v - deadZone) * 2, 0x100);
int raceinput_wheelPedal(int value, int deadZone)
{
    int v = 0xFF - (value & 0xFF);              /* the pedal pressed: 0 .. 0xFF */

    (void)deadZone;
    return (v * 0x100 + 0x7F) / 0xFF;
}

// FUNC 0x002115F8 RaceInput::readWheel
//   USB wheel types 1..3: the wheel's function bits, steering from channel
//   base+0 (centre 0x200, x9, clamped to +-0x1000), pedals from channels
//   base+1 / +2 (dead zones 0x4B / 0x50).  The wheel's channels start at
//   0x10 when a pad is there too, else at 0.
//   The enhanced engine (backend.md B1; Lupo 2 L2 0x0021DCA8): type 4, the
//   Driving Force Pro (only Lupo 2's IOP module reports it), device 6 with
//   its 14-bit wheel, 0x1000 - x / 2 (centre 0x2000).
//   PC: the local players never have the pad and the wheel merged here
//   (engine/input T478: one device each, gt3pc.ini [WheelN] Input).
void RaceInput::readWheel(CarInput *in)
{
    u32 bits = getButtons();
    int usbType = raceinput_usbType(this);
    int base = (rawPSType() != PS_PAD_NONE) ? CHAN_PAD_COUNT : 0;
    int steer;
    int pedal;

    if (usbType >= 5)                           /* the enhanced engine: type 4 too (GT3 >= 4) */
        return;
    if (usbType == USB_WHEEL_NONE)
        return;
    raceinput_digitalButtons(in, bits);
    if (usbType == USB_WHEEL_DFP) {
        /* Lupo 2 (L2 0x0021DD80) */
        steer = 0x1000 - (int)(getAnalog(base) >> 1);
        if (steer < -0x1000)
            steer = -0x1000;
        else if (steer >= 0x1001)
            steer = 0x1000;
        in->device = STEER_WHEEL_DFP;
        in->steer = steer;
        in->flags |= CARINPUT_ANALOG_STEER;
    } else {
        /* PC (2026-11-20, user: "add an option to disable it, so steering is
         * directly dictated by the wheel's steering input.  Also, a steering
         * input curve"): the wheel's position (-1..1, + left) through the
         * player's curve ([WheelN] SteeringCurve, platform/input_map.h
         * apply_steer_curve) - before the input is recorded, so a replay or ghost
         * steers as the race did; then, with the steering filter on, GT3's x 9
         * (the car's lock at 8/9 of the wheel's travel: 0x200 x 9 = 0x1200,
         * held to 0x1000) as STEER_WHEEL, off the whole travel to the full
         * lock (x 8) as STEER_WHEEL_DIRECT (T342: the lock straight from it).
         * Retail: steer = (0x200 - x) x 9, held to +-0x1000. */
        const int player = port >= 0 && port <= 1 ? port : 0;
        const bool filter = input_map::steer_filter(player);
        f32 pos = (f32)(-(int)getAnalog(base) + 0x200) / 512.0f;
        if (pos > 1.0f)
            pos = 1.0f;
        else if (pos < -1.0f)
            pos = -1.0f;
        pos = input_map::apply_steer_curve(pos, input_map::steer_curve(player));
        steer = (int)(pos * (filter ? 0x1200 : 0x1000) + (pos < 0.0f ? -0.5f : 0.5f));
        if (steer < -0x1000)
            steer = -0x1000;
        else if (steer >= 0x1001)
            steer = 0x1000;
        in->device = filter ? STEER_WHEEL : STEER_WHEEL_DIRECT;
        in->steer = steer;
        in->flags |= CARINPUT_ANALOG_STEER;
    }
    pedal = raceinput_wheelPedal(getAnalog(base | 1), 0x4B);
    if (pedal > 0) {
        in->throttle = pedal << 4;
        in->flags |= CARINPUT_ANALOG_THROTTLE;
    }
    pedal = raceinput_wheelPedal(getAnalog(base | 2), 0x50);
    if (pedal > 0) {
        in->brake = pedal << 4;
        in->flags |= CARINPUT_ANALOG_BRAKE;
    }
}

/* The enhanced engine (backend.md B2, race_logic.md C5; Lupo 2 L2
 * 0x0021CE60): the controller's own centring spring off while racing
 * (mode 0) and on in the pause and outside the race (mode 1), so in a race
 * only the game's force feedback turns the wheel.  Lupo 2 also keeps the
 * mode at RaceInput +0x148, which nothing reads (its getter L2 0x0021CEA4
 * has no caller): not kept, RaceInput keeps its size (RaceSplitBattle holds
 * an array of them).  Its second helper (L2 0x0021CEA8, the port's
 * setDamper) has no caller either: not ported. */
void RaceInput::setMode(int mode)
{
    if (Control::port != NULL)
        Control::port->enableSpring(mode != 0);
}

/* PC (2026-11-20, user: "at the start of a race, an overlay popup should
 * appear alerting the user if FFB is off and while it is taking inputs from
 * the wheel device (Both connected under controller mode wouldn't pop
 * this)"): a live human input whose device is the wheel - its Control a USB
 * wheel's (T478: with both connected and [WheelN] Input=controller it is the
 * pad's) - with the option sendVibration reads off: the notice. */
void RaceInput::pcFfbNotice()
{
    if (enabled == 0 || replayMode != REPLAY_LIVE || port < 0 || port > 1)
        return;
    if (rawUSBType() == USB_WHEEL_NONE)
        return;
    if ((port == 0) ? getGameStatus()->getGameOptions()->forceFeedback1p
                    : getGameStatus()->getGameOptions()->forceFeedback2p)
        return;
    pc_ffb_notice(port);
}

/* A disabled RaceInput (.data 0x002DFB58, zero-filled: an object with a
 * constructor), right after g_002DFB18 as in retail. */
RaceInput g_002DFB58(0);                        /* 0x002DFB58 */

/* Compiler-generated: egcs's static destructor of g_002DFB58 (.dtors entry
 * 0x0028F240), ~RaceInput() with flags 2. */
// FUNC 0x00211750 T413_static_destroy (_GLOBAL_$D$g_002DFB18)
/* implicit: ~RaceInput (the destructor) */

/* Compiler-generated: egcs's static constructor of g_002DFB58 (.ctors entry
 * 0x0028F168), RaceInput(0) on it. */
// FUNC 0x00211778 T413_static_init (_GLOBAL_$I$g_002DFB18)
/* implicit: RaceInput (the constructor) */

// FUNC 0x002117A0 RaceInput::maxFrames
//   36000: the frames of 10 minutes at 60 Hz (T318's frames comment says
//   the same).  No callers.
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return 36000; */

// FUNC 0x002117A8 RaceInput::setSnapshots
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: snapshots = p; */

// FUNC 0x002117B0 RaceInput::getSnapshot
//   0x1B00 bytes each (T313's OneLapGhostInfo; u095 does not include T313,
//   so the stride is written out).
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return (OneLapGhostInfo *)((u8 *)snapshots + n * 0x1B00); */

// FUNC 0x002117C8 RaceInput::getSnapshots
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return snapshots; */

// FUNC 0x002117D0 RaceInput::getLap
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return lap; */

// FUNC 0x002117D8 RaceInput::setLap
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: lap = m; */

// FUNC 0x002117E0 RaceInput::getBestLap
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return bestLap; */

// FUNC 0x002117E8 RaceInput::setBestLap
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: bestLap = m; */

// FUNC 0x002117F0 RaceInput::saveSize
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return rec.saveSize(multi); */
/* implicit: saveSize (the copy's call) */

// FUNC 0x00211810 RaceInput::save
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: rec.save(buf, multi); */
/* implicit: save (the copy's call) */

// FUNC 0x00211830 RaceInput::load
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return rec.load(src, multi); */
/* implicit: load (the copy's call) */

// FUNC 0x00211850 RaceInput::getSlot
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return rec.getSlot(); */
/* implicit: getSlot (the copy's call) */

// FUNC 0x00211870 RaceInput::frameCount
//   The recorder's (Recorder::frameCount), not Manager::frameCount.
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return rec.rec.frameCount(); */
/* implicit: frameCount (the copy's call) */

// FUNC 0x00211890 RaceInput::getFrames
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return rec.rec.frames; */

// FUNC 0x00211898 RaceInput::isFinished
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return rec.rec.finished; */

// FUNC 0x002118A0 RaceInput::stop
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: rec.rec.stop(); */
/* implicit: stop (the copy's call) */

// FUNC 0x002118C0 RaceInput::record
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: rec.rec.record(in); */
/* implicit: record (the copy's call) */

// FUNC 0x002118E0 RaceInput::skip
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: rec.rec.skip(n, keep); */
/* implicit: skip (the copy's call) */

// FUNC 0x00211900 RaceInput::play
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: rec.rec.play(out); */
/* implicit: play (the copy's call) */

// FUNC 0x00211920 RaceInput::reset
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: rec.reset(); */
/* implicit: reset (the copy's call) */

// FUNC 0x00211940 RaceInput::attachBuffer
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: rec.attachBuffer(i); */
/* implicit: attachBuffer (the copy's call) */

// FUNC 0x00211960 RaceInput::getRecord
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return &rec; */

// FUNC 0x00211968 RaceInput::setPort
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: port = p; */

// FUNC 0x00211970 RaceInput::setHold
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: hold = h; */

// FUNC 0x00211978 RaceInput::getVibration
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return vibration; */

// FUNC 0x00211980 RaceInput::setVibration
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: vibration = on; */

// FUNC 0x00211988 RaceInput::getRecordMode
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return rec.mode; */

// FUNC 0x00211990 RaceInput::setRecordMode
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: rec.mode = mode; */

// FUNC 0x00211998 RaceInput::getReplayMode
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return replayMode; */

// FUNC 0x002119A0 RaceInput::getHold
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return hold; */

// FUNC 0x002119A8 RaceInput::isEnabled
/* inline in game/race/core/T413_race_input.h (no callers: the original expanded it
 * where it was used); egcs emits this copy with the vtable: return enabled; */

