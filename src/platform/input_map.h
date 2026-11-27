/* input_map.h - the control mappings of GT3 PC (gt3pc.ini [Controller1],
 * [Controller2], [Wheel1], [Wheel2], [DebugCamera]).
 *
 * Shared by the game and the launcher (no SDL, no game headers): the
 * launcher's Controls tab edits the mappings, the game reads them at start
 * (platform/pad_pc.cpp: the controllers, platform/wheel.cpp: the wheels) and
 * the in-game settings menu changes the dead zones.
 *
 * Four profiles: Controller 1 / 2, the DualShock 2s of the game's pad ports
 * 1 / 2, and Wheel 1 / 2, the Logitech Driving Forces (USB 0x46D:0xC294) of
 * its USB wheel ports 1 / 2 (player 1 / 2).  Each profile maps its virtual
 * controls (the DualShock 2's buttons, D-pad and stick directions; the
 * Driving Force's steering, pedals, 12 buttons and D-pad; and either's
 * chase camera directions, CameraLeft / Right / Up / Down: the free orbit,
 * game/app/pc_freecam.h - by default a controller's right stick and
 * Controller 1's J / L / I / K, none on a wheel) from two slots:
 *   slot 0  the device: Controller N's SDL gamepad (the Nth connected; buttons
 *           and axes by their position, Xbox-style: south = A / Cross), or
 *           Wheel N's SDL joystick (raw buttons, axes and hats); or another
 *           device the binding names (`@<name>', below: a wheel's pedals of
 *           their own, a shifter, a button box, pedals beside a gamepad);
 *   slot 1  the keyboard and the mouse.
 * The control's value is the larger of the two (0..1).  A fifth profile,
 * Debug camera, maps the debug camera's controls (DebugControl) the same way
 * from the first gamepad (Controller 1's) and the keyboard / mouse, with a
 * controller's dead zones (platform/debug_camera.h).  PC (2026-11-09): each
 * controller and wheel profile maps a Clutch too (`Clutch=', none by
 * default), and two more profiles, H-Shifter 1 / 2 ([HShifter1] /
 * [HShifter2]: Neutral, Gear1 .. Gear9), map the players' H-pattern
 * shifters, one binding a gate, from any device or the keyboard / mouse
 * (`Gear3=joy:button10@<shifter>;') - the realistic gear shift's clutch
 * mode (game/app/pc_clutch.h).
 *
 * In gt3pc.ini each control is `Key=slot0;slot1', a binding or nothing in
 * each (a missing key: the default; `Key=;' none at all):
 *   button:<name>          a gamepad button (south, east, west, north, back,
 *                          start, leftstick, rightstick, leftshoulder,
 *                          rightshoulder, dpup, dpdown, dpleft, dpright, misc1,
 *                          paddle1..4, touchpad, misc2..6; SDL3's order)
 *   axis:<name>+ / -       one half of a gamepad axis (leftx, lefty, rightx,
 *                          righty, lefttrigger, righttrigger; y down is +)
 *   joy:button<n>          a joystick button (0..127)
 *   joy:axis<n>+ / -       one half of a joystick axis (centred at rest)
 *   joy:axis<n>+full / -full   the whole axis, from its - end (+full) or its
 *                          + end (-full) at rest: a pedal
 *   joy:hat<n>up / right / down / left   a joystick hat's direction
 *   key:<name>             a key (Windows virtual key: A..Z, 0..9, F1..F24,
 *                          Up, Down, Left, Right, Enter, Space, Backspace, Tab,
 *                          Shift, Ctrl, Alt, Numpad0..9, ...; or 0xNN)
 *   mouse:left / right / middle / x1 / x2 / wheelup / wheeldown
 *   mouse:moveleft / moveright / moveup / movedown   the mouse's movement
 *                          (the Debug camera's look only)
 * A slot 0 binding may end in `@<name>': read from the connected joystick
 * SDL names so (the first of that name; button: / axis: through SDL's
 * gamepad mapping of it, joy: raw) instead of the profile's own device -
 * `Gas=joy:axis2+full@Fanatec ClubSport Pedals V3;', `Cross=joy:axis1+full@
 * <pedals>;key:Z'.  Without it, the profile's own (the line splits at its
 * last `;', so a name may hold one).  A joystick a wheel's binding names is
 * that wheel's device, never an automatic wheel itself (Device=auto).
 * Besides, per profile:
 *   Controller:  LeftStickDeadzone=0, RightStickDeadzone=0, TriggerDeadzone=0
 *                (percent, 0..50: the sticks' radially, the triggers' at the
 *                start of their travel; the rest rescaled to the full range)
 *   Wheel:       Device=auto (the joystick's name; auto: the Nth joystick
 *                SDL knows as a wheel), SteeringDeadzone=0, GasDeadzone=0,
 *                BrakeDeadzone=0, ClutchDeadzone=0 (PC 2026-11-10) (percent,
 *                0..50, on the virtual steering and pedals), Input=wheel | controller (what the player - 1 for
 *                Wheel 1, 2 for Wheel 2 - plays with while both the wheel and
 *                the controller are connected; with one of them, that one:
 *                engine/input T478)
 *
 * [Hotkeys]: the game window's own keys (platform/window.cpp), each a key
 * with exactly the modifiers held with it:
 *   SettingsMenu=F2  SystemMenu=Escape  FrameMonitor=F3  Fullscreen=Alt+Enter
 *   Pause=F5  Resume=F6  FlickerCapture=F8  Trace=F9  DebugCamera=F7  FrameCapture=F11
 *   SongMenu=M;button:back  PreviousSong=Comma;button:dpup
 *   NextSong=Period;button:dpdown   (the race's song changer: platform/song_osd.h)
 *   CalibrationUp=Up  CalibrationDown=Down  CalibrationLeft=Left  CalibrationRight=Right
 *   CalibrationSave=F4  CalibrationCancel=Delete  CalibrationReset=0
 *                        (the cockpit camera's calibration: platform/cockpit_osd.h;
 *                        these act only while it is on, so their keys may be a
 *                        control's too - the arrows are the D-pad's: hotkey_shares_keys)
 *   TextureMenu=F10      (PC 2026-11-13: the texture replacement interface,
 *                        platform/texture_menu.h)
 *   TextureRefresh=Ctrl+F10   (the texture folders read again: the files
 *                        that changed loaded anew)
 *   TextureToggle=F12    (the replacement textures on / off: a replacement
 *                        compared with the game's own)
 *   Screenshot=F1        (PC 2026-11-15: the picture at the game's own
 *                        resolution as a PNG in screenshots\, platform/
 *                        screenshot.h; F12 was taken, and Windows 11 gives
 *                        Print Screen to its Snipping Tool)
 * `Key=Ctrl+Shift+F5': Ctrl+, Alt+, Shift+ before a key's name (key:<name>'s;
 * not a modifier alone); `Key=none' (or empty) none; a missing or unreadable
 * value the default.  A hotkey's key does not reach the game, and the
 * launcher keeps the controls' bindings off the hotkeys' keys.
 * PC (2026-11-02): after a `;' a device's binding too (a slot 0 binding's
 * text: `SettingsMenu=F2;button:misc1', `Pause=F5;joy:button9@<name>'): a
 * gamepad's button or axis on any connected controller, a joystick's
 * (button: / axis: / joy:) on the device it names, else on either wheel;
 * pressed (an axis past half) it is the hotkey too.  Without the `;' the
 * default (none but the song changer's: Select, D-pad up / down); `Key=F2;'
 * none.  A device's press also reaches the game.  A new hotkey's default key
 * that a control's keyboard slot has (gt3pc.ini written before it) is none. */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace input_map {

/* ---- the profiles and their controls ---------------------------------- */

/* PC (2026-11-09): H-Shifter 1 / 2 ([HShifter1], [HShifter2]): the players'
 * H-pattern shifters, virtual devices of their own - Neutral, 1st .. 9th and
 * Reverse (HShifterControl), one binding each from any device (a joystick's, a
 * gamepad's, a key, the mouse: slot 0 takes them all, slot 1 is not used;
 * a device's binding always names it, `@<name>'); the realistic gear shift's
 * clutch mode reads them (game/app/pc_clutch.h, platform/clutch_input.h). */
enum { kController1, kController2, kWheel1, kWheel2, kDebugCamera, kHShifter1, kHShifter2, kProfileCount };
inline bool is_wheel(int profile) { return profile == kWheel1 || profile == kWheel2; }
inline bool is_hshifter(int profile) { return profile == kHShifter1 || profile == kHShifter2; }
const char *profile_section(int profile);   /* "Controller1" .. "Wheel2", "DebugCamera" */
const char *profile_name(int profile);      /* "Controller 1" .. "Wheel 2", "Debug camera" */

/* The controller's controls (a DualShock 2's), then the chase camera's
 * (not the DualShock 2's: its free orbit, game/app/pc_freecam.h; the
 * launcher lists them first: control_in_order). */
enum PadControl {
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE,
    PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_L3, PAD_R3,
    PAD_SELECT, PAD_START,
    PAD_LSTICK_UP, PAD_LSTICK_DOWN, PAD_LSTICK_LEFT, PAD_LSTICK_RIGHT,
    PAD_RSTICK_UP, PAD_RSTICK_DOWN, PAD_RSTICK_LEFT, PAD_RSTICK_RIGHT,
    /* PC (2026-11-09): the clutch (not the DualShock 2's: the realistic gear
     * shift's clutch mode, game/app/pc_clutch.h; a pedal, 0 released) */
    PAD_CLUTCH,
    PAD_CAM_LEFT, PAD_CAM_RIGHT, PAD_CAM_UP, PAD_CAM_DOWN,
    kPadControlCount
};
/* The wheel's controls (a Driving Force): buttons in its USB report's order
 * (WHEEL_CROSS + i = report button i); then the chase camera's, as the
 * controller's. */
enum WheelControl {
    WHEEL_STEER_LEFT, WHEEL_STEER_RIGHT, WHEEL_GAS, WHEEL_BRAKE,
    WHEEL_CROSS, WHEEL_SQUARE, WHEEL_CIRCLE, WHEEL_TRIANGLE,
    WHEEL_R1, WHEEL_L1, WHEEL_R2, WHEEL_L2,
    WHEEL_SELECT, WHEEL_START, WHEEL_R3, WHEEL_L3,
    WHEEL_UP, WHEEL_DOWN, WHEEL_LEFT, WHEEL_RIGHT,
    WHEEL_CLUTCH,                           /* PC (2026-11-09): the clutch (PAD_CLUTCH's) */
    WHEEL_CAM_LEFT, WHEEL_CAM_RIGHT, WHEEL_CAM_UP, WHEEL_CAM_DOWN,
    kWheelControlCount
};
/* PC (2026-11-09): an H-Shifter's gates (is_hshifter); the reverse's last
 * (added after: the launcher lists it after Neutral, control_in_order). */
enum HShifterControl {
    HS_NEUTRAL, HS_GEAR1, HS_GEAR2, HS_GEAR3, HS_GEAR4, HS_GEAR5, HS_GEAR6, HS_GEAR7, HS_GEAR8, HS_GEAR9,
    HS_REVERSE,
    kHShifterControlCount
};
/* A profile's clutch control (-1: none, the debug camera's and the
 * H-Shifters'). */
inline int clutch_control(int profile)
{
    return profile <= kController2 ? PAD_CLUTCH : profile <= kWheel2 ? WHEEL_CLUTCH : -1;
}
/* A profile's chase camera controls: left, right, up, down from this one. */
inline int camera_control(int profile) { return is_wheel(profile) ? WHEEL_CAM_LEFT : PAD_CAM_LEFT; }
constexpr int kCameraControls = 4;
/* The debug camera's controls ([DebugCamera], profile kDebugCamera: the
 * first gamepad and the keyboard / mouse; platform/debug_camera.h). */
enum DebugControl {
    DEBUG_FORWARD, DEBUG_BACK, DEBUG_LEFT, DEBUG_RIGHT, DEBUG_UP, DEBUG_DOWN,
    DEBUG_LOOK_UP, DEBUG_LOOK_DOWN, DEBUG_LOOK_LEFT, DEBUG_LOOK_RIGHT,
    DEBUG_FASTER, DEBUG_SLOWER, DEBUG_FOV_UP, DEBUG_FOV_DOWN,
    DEBUG_LOCK,                             /* the camera held, the car the player's again (a press each way) */
    DEBUG_Y_LOOK,                           /* forward and back follow the pitch (a press each way) */
    DEBUG_GROUND,                           /* the height kept above the road (a press each way) */
    kDebugControlCount
};
constexpr int kWheelButtons = 12;
constexpr int kMaxControls = kPadControlCount > kWheelControlCount ? kPadControlCount : kWheelControlCount;
static_assert(kDebugControlCount <= kMaxControls && kHShifterControlCount <= kMaxControls, "a profile's controls");

/* What a control takes: a button (pressed or not; an analog source counts
 * from halfway, its travel as the pressure), a direction of a stick or the
 * steering (0..1), a pedal (0..1). */
enum class ControlKind : uint8_t { button, direction, pedal };
struct ControlInfo {
    const char *key;                        /* the ini key */
    const char *name;                       /* the launcher's label */
    ControlKind kind;
};
int control_count(int profile);
const ControlInfo &control(int profile, int i);
/* The control in the launcher's row `row' (0 .. control_count - 1): a
 * controller's Clutch, then its chase camera's, then the others in their
 * order; a wheel's chase camera's, then the others in their order with the
 * Clutch after the Brake pedal (PC 2026-11-10); an H-Shifter's: Neutral,
 * Reverse, 1st .. 9th. */
int control_in_order(int profile, int row);

/* ---- bindings -------------------------------------------------------- */

enum class Source : uint8_t {
    none,
    pad_button,                             /* index: SDL_GamepadButton */
    pad_axis,                               /* index: SDL_GamepadAxis; dir +1 / -1 */
    joy_button,                             /* index: the button */
    joy_axis,                               /* index: the axis; dir +1 / -1 a half, +2 / -2 the whole (+full / -full) */
    joy_hat,                                /* index: the hat; dir: SDL_HAT_* bit (1 up, 2 right, 4 down, 8 left) */
    key,                                    /* index: the virtual key (VK_LBUTTON .. VK_XBUTTON2: the mouse's buttons) */
    mouse_wheel,                            /* dir +1 up, -1 down (a notch: a tap) */
    mouse_motion                            /* index 0 x / 1 y, dir -1 left / up, +1 right / down: the
                                               mouse's movement (value 0: the debug camera counts it) */
};
struct Binding {
    Source source = Source::none;
    uint8_t index = 0;
    int8_t dir = 0;
    uint8_t device = 0;                     /* pad_* / joy_*: the device it names (device_name), 0 the profile's own */
};
constexpr int kSlots = 2;                   /* 0 the device, 1 the keyboard / mouse */
bool slot_takes(int slot, Source s);        /* slot 0: pad_* / joy_*; slot 1: key / mouse_wheel / mouse_motion */
/* Slot `slot' of profile `profile' takes source s: slot_takes's; PC
 * (2026-11-09) an H-Shifter's slot 0 any source (no mouse movement), its
 * slot 1 none. */
bool slot_takes(int profile, int slot, Source s);

/* The devices bindings name (`@<name>'), numbered from 1 in the order met,
 * for the whole process (Binding::device). */
int device_id(const char *name);            /* found (case aside) or added; 0 for "" or too many */
const char *device_name(int id);            /* "" for 0 or an unknown id */
int device_count();                         /* the ids 1 .. device_count() */

/* The ini text of a binding ("" for none; `@<name>' after it for a device it
 * names) and back (false: not a binding; "" and "none" are none). */
void format(const Binding &b, char *out, size_t n);
bool parse(const char *text, Binding *out);
/* A label for the launcher ("Left stick up", "Button 3", "Axis 2 (whole, +
 * pressed)", the key's name in the keyboard's layout; " on <name>" after it
 * for a device it names; "" for none). */
void describe(const Binding &b, char *out, size_t n);

/* The gamepad buttons / axes SDL3 has (Source::pad_*'s index range). */
constexpr int kPadButtons = 26, kPadAxes = 6;
constexpr int kPadButtonGuide = 5;          /* SDL_GAMEPAD_BUTTON_GUIDE: the menus (gamepad.h), never mapped */
constexpr int kPadAxisLeftTrigger = 4, kPadAxisRightTrigger = 5;
/* An axis moved far enough from its rest value to be taken as a binding
 * (the launcher's capture): a half of a centred axis, the whole of one that
 * rests at an end (|rest| > 16384: a pedal), the direction it moved.  joy:
 * a joystick axis (else a gamepad axis: always a half). */
bool capture_axis(bool joy, int axis, int rest, int now, Binding *out);
/* PC (2026-11-23): the same for a wheel profile's SteerLeft / SteerRight
 * from a joystick axis: always a half, the one the axis moved towards,
 * taken once it moved kSteerCaptureMove from where it was (a tenth of its
 * whole travel).  A user's report (a Fanatec direct drive base): after
 * SteerLeft the wheel stayed where it was turned to - no spring of its own -
 * so SteerRight began from past half lock, which capture_axis reads as an
 * axis at rest at an end and took whole: `SteerLeft=joy:axis0-;
 * SteerRight=joy:axis0+full'.  That steers half right with the wheel
 * centred, and the menus then hold "right" for good, which keeps every
 * button out (MenuControl::updateKeys): no confirm, no movie skip. */
constexpr int kSteerCaptureMove = 6554;
bool capture_steer_axis(int axis, int rest, int now, Binding *out);

/* ---- a profile ------------------------------------------------------- */

/* controller: left stick, right stick, triggers; wheel: steering, gas, brake
 * and (PC 2026-11-10) clutch - a controller's fourth not used */
constexpr int kDeadzones = 4;
constexpr int kWheelClutchDeadzone = 3;     /* PC (2026-11-10): a wheel's ClutchDeadzone */
constexpr int kDeadzoneMax = 50;
/* The profile's dead zones: a wheel's four, a controller's and the debug
 * camera's three, an H-Shifter's none (PC 2026-11-09 / 10). */
inline int deadzone_count(int profile) { return is_hshifter(profile) ? 0 : is_wheel(profile) ? 4 : 3; }
inline bool has_deadzones(int profile) { return deadzone_count(profile) > 0; }
constexpr int kStickDeadzoneDefault = 15;   /* Controller 1 / 2: both sticks' (the others 0) */
const char *deadzone_key(int profile, int i);
const char *deadzone_name(int profile, int i);      /* the launcher's / settings menu's label */

/* PC (2026-11-20, user: "Force feedback for the virtual wheels ... FFB
 * settings: Strength, etc."): a wheel's force feedback (platform/wheel_ffb.h),
 * [WheelN] FfbStrength=25 (percent, 0..200: the game's steering force; 0
 * none; 25 by default, the user's), FfbSpring=100 (0..300: the menus' centring spring, the game's own
 * at 100), FfbMinimum=0 (0..30: the least force a non-zero one is lifted to,
 * for a wheel that does not move below it), FfbInvert=off (on: every force
 * the other way).  PC (2026-11-22, a user's report: "Shortly after the start
 * of a race (about 30-40 seconds or so) with certain wheels (I am using a
 * Simagic Alpha Evo) force feedback is lost.  There is a similar issue on
 * PCSX2 which is handled via an option called 'workaround for intermittent
 * FFB loss'"): FfbLossWorkaround=off (on: the steering force started again
 * with each one the game sends, as PCSX2's FfbDropoutWorkaround:
 * platform/wheel_ffb.h). */
enum FfbSetting { kFfbStrength, kFfbSpring, kFfbMinimum, kFfbInvert, kFfbLossWorkaround, kFfbSettings };
const char *ffb_key(int i);                 /* the ini key: "FfbStrength" .. */
const char *ffb_name(int i);                /* the launcher's label (N_) */
int ffb_default(int i);
int ffb_max(int i);                         /* (the least is 0; the switches' 1) */
/* PC (2026-11-22): an on / off setting (FfbInvert, FfbLossWorkaround: the
 * ini's "on" / "off") */
inline bool ffb_is_switch(int i) { return i == kFfbInvert || i == kFfbLossWorkaround; }

/* PC (2026-11-20, user: "There is a steering input filter for the wheel, add
 * an option to disable it, so steering is directly dictated by the wheel's
 * steering input.  Also, a steering input curve ... from a slow ramp up at
 * the end, to linear, to a fast ramp up"): a wheel's [WheelN]
 * SteeringFilter=on (GT3's: the car's lock shrinking with speed, following
 * slides, changing at 90 deg/s) | off (the car's full lock straight from the
 * wheel: game/car/physics/T342, INPUT_DEV_WHEEL_DIRECT) and SteeringCurve=0
 * (-100..100: below 0 a slow ramp up - fine near the centre, steep at the
 * end - 0 linear, above 0 a fast ramp up; apply_steer_curve). */
constexpr int kSteerCurveMax = 100;
/* The steering input x (-1..1) through curve c (-100..100): for c > 0
 * (1 + k) x / (1 + k |x|), for c < 0 its mirror over the diagonal x / (1 + k
 * - k |x|), k = 9 |c| / 100 (the slope at the centre 10 x or 1 / 10 at the
 * ends of the range, finite everywhere); 0 the input as it is. */
float apply_steer_curve(float x, int c);

struct Profile {
    Binding map[kMaxControls][kSlots];
    int deadzone[kDeadzones] = {0, 0, 0, 0};    /* (defaults: kStickDeadzoneDefault for a controller's sticks) */
    char device[128] = "";                  /* wheels: the joystick's name ("": auto) */
    bool controller = false;                /* wheels: Input=controller (else wheel) */
    int ffb[kFfbSettings] = {25, 100, 0, 0, 0}; /* wheels: the force feedback's (PC 2026-11-20; 11-22 the workaround) */
    bool steerFilter = true;                /* wheels: SteeringFilter (PC 2026-11-20) */
    int steerCurve = 0;                     /* wheels: SteeringCurve (PC 2026-11-20) */
};
void defaults(int profile, Profile *out);
/* gt3pc.ini at `ini' (a full path): a missing value the default. */
void load(const char *ini, int profile, Profile *out);
/* All of it written (the launcher). */
void save(const char *ini, int profile, const Profile &p);
/* A binding of the profile names the device `name' (case aside). */
bool names_device(const Profile &p, const char *name);
/* PC (2026-11-23): the device a wheel profile's steering names (its
 * SteerLeft / SteerRight bound to a joystick `@<name>'), "" when it names
 * none.  That joystick is the wheel itself, whatever its Device says while
 * that is auto: the game (gamepad.cpp wheel_for) and the launcher take it
 * as the profile's wheel.  A user's gt3pc.ini had every binding of Wheel 1
 * written with its own wheel base's name (platform/joy_names.h), which kept
 * the base from being a wheel at all. */
const char *steer_device(const Profile &p);

/* ---- the sources ----------------------------------------------------- */

/* What a binding reads: the devices' raw state, the keyboard and mouse.
 * pad / joy: nullptr when absent. */
struct PadSource {
    uint32_t buttons;                       /* bit = SDL_GamepadButton */
    int16_t axes[kPadAxes];                 /* SDL's: sticks -32768..32767, triggers 0..32767 */
};
struct JoySource {
    int buttonCount, axisCount, hatCount;
    const uint8_t *buttons;
    const int16_t *axes;
    const uint8_t *hats;                    /* SDL_HAT_* bits */
};
/* A device bindings name (Binding::device): its gamepad mapping (nullptr
 * when SDL maps it as none) and its joystick; both nullptr while it is not
 * connected. */
struct DeviceSource {
    const PadSource *pad;
    const JoySource *joy;
};
/* The devices the game opens for the bindings (ids 1 .. kMaxDevices; a later
 * id reads nothing). */
constexpr int kMaxDevices = 16;
struct Sources {
    const PadSource *pad;                   /* the profile's own device */
    const JoySource *joy;
    bool (*key)(int vk);                    /* the key (or mouse button) is down; nullptr: none */
    int wheel;                              /* mouse wheel notches since the last read: bit 0 up, bit 1 down */
    const DeviceSource *devices;            /* [Binding::device - 1]; nullptr: none */
    int deviceCount;
};
/* The binding's value 0..1 (a button 0 / 1). */
float value(const Binding &b, const Sources &s);
/* A binding's source is analog (an axis): a button taken from it counts
 * from halfway (a trigger from the slightest press: gamepad.h). */
bool is_analog(const Binding &b);

/* ---- the game's (loaded once; the dead zones changed in the game) ------ */

/* Reads the four profiles (after paths::init; the game's gt3pc.ini). */
void init(const char *ini);
const Profile &profile(int p);
/* The dead zone in percent; set: held to 0..kDeadzoneMax, written to
 * gt3pc.ini at once.  Any thread. */
int deadzone(int p, int i);
void set_deadzone(int p, int i, int percent);
/* A dead zone d (0..1) applied to a value 0..1: 0 inside, the rest
 * stretched to 0..1. */
float apply_deadzone(float v, float d);
/* Player `player' (0, 1) plays with the controller while both it and the
 * wheel are connected ([WheelN] Input=controller; else the wheel); set:
 * written to gt3pc.ini at once.  Any thread. */
bool plays_with_controller(int player);
void set_plays_with_controller(int player, bool on);
/* PC (2026-11-20): wheel `player''s (0, 1) force feedback setting i
 * (FfbSetting); set: held to 0..ffb_max, written to gt3pc.ini at once.  Any
 * thread. */
int ffb(int player, int i);
void set_ffb(int player, int i, int v);
/* PC (2026-11-20): wheel `player''s steering filter (on: GT3's) and curve
 * (-100..100); set: written to gt3pc.ini at once.  Any thread. */
bool steer_filter(int player);
void set_steer_filter(int player, bool on);
int steer_curve(int player);
void set_steer_curve(int player, int c);

/* ---- the hotkeys ([Hotkeys]) ------------------------------------------ */

enum Hotkey {
    kHotSettingsMenu, kHotSystemMenu, kHotFrameMonitor, kHotFullscreen,
    kHotPause, kHotResume, kHotFlickerCapture, kHotTrace, kHotDebugCamera,
    kHotSongMenu, kHotPreviousSong, kHotNextSong,           /* PC: the race's song changer */
    kHotFrameCapture,                                       /* PC (2026-11-06): the frame capture (pgl_capture.h) */
    /* PC (2026-11-08): the cockpit camera's calibration (cockpit_osd.h): the
     * axis before / after, a step down / up, save, cancel, the axis to 0;
     * they act only while it is on (hotkey_shares_keys) */
    kHotCalibrationUp, kHotCalibrationDown, kHotCalibrationLeft, kHotCalibrationRight,
    kHotCalibrationSave, kHotCalibrationCancel, kHotCalibrationReset,
    kHotTextureMenu,                                        /* PC (2026-11-13): the texture interface (texture_menu.h) */
    kHotTextureRefresh,                                     /* PC (2026-11-13): its refresh, the panel shown or not */
    kHotTextureToggle,                                      /* PC (2026-11-13): the replacements on / off */
    kHotScreenshot,                                         /* PC (2026-11-15): a screenshot (screenshot.h) */
    kHotkeyCount
};
struct KeyCombo {
    uint8_t vk = 0;                         /* the virtual key; 0: none */
    bool ctrl = false, alt = false, shift = false;
};
inline bool operator==(const KeyCombo &a, const KeyCombo &b)
{
    return a.vk == b.vk && (a.vk == 0 || (a.ctrl == b.ctrl && a.alt == b.alt && a.shift == b.shift));
}
const char *hotkey_key(int h);              /* the ini key: "SettingsMenu" .. */
const char *hotkey_name(int h);             /* the launcher's label */
KeyCombo hotkey_default(int h);
/* PC (2026-11-08): hotkey h acts only while its mode is on (the cockpit
 * camera's calibration), so its key may be a control's too: load_hotkeys
 * keeps its default then, and the launcher lets a control and it share a
 * key. */
bool hotkey_shares_keys(int h);
/* A modifier key (Shift, Ctrl, Alt, either side's): never a combo's key. */
bool is_modifier(int vk);
/* The ini text of a combo ("Alt+Enter"; "none") and back (false: not one;
 * "" and "none" are none). */
void format_combo(const KeyCombo &k, char *out, size_t n);
bool parse_combo(const char *text, KeyCombo *out);
/* A label for the launcher: the key's name in the keyboard's layout after
 * the modifiers ("Ctrl+Esc"; "" for none). */
void describe_combo(const KeyCombo &k, char *out, size_t n);
/* Hotkey h's device default (none: Binding's source none). */
Binding hotkey_device_default(int h);
/* gt3pc.ini at `ini': all of them (a missing or unreadable one the
 * default; a missing one whose default key, without modifiers, a control's
 * keyboard slot has in any profile: none), their devices in dev (unless
 * nullptr); one written. */
void load_hotkeys(const char *ini, KeyCombo out[kHotkeyCount], Binding dev[kHotkeyCount] = nullptr);
void save_hotkey(const char *ini, int h, const KeyCombo &k, const Binding &dev);

/* The game's (read by init). */
const KeyCombo &hotkey(int h);
const Binding &hotkey_device(int h);
/* The key vk going down with those modifiers is hotkey h. */
bool is_hotkey(int h, int vk, bool ctrl, bool alt, bool shift);
/* Hotkey h for the overlays: its ini text, Escape as "Esc" ("F2",
 * "Alt+Enter"; "" for none). */
void hotkey_text(int h, char *out, size_t n);

} // namespace input_map
