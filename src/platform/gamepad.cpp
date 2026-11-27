/* gamepad.cpp - game controllers and wheels of GT3 PC, through SDL3
 * (gamepad.h).
 *
 * Windows: SDL is started and polled on one thread of its own (SDL's device
 * notifications belong to the thread that starts it): every kPollMs it
 * handles the plug / unplug events, reads each open pad, wheel and named
 * device into the shared state, drives the motors and watches the Guide
 * buttons (PC: and the hotkeys' devices: hotkeys).
 *
 * Elsewhere SDL is linked directly and belongs to the main thread: the
 * window started it (window::create, the video), init adds the controllers
 * on that thread, and the window's loop hands the plug / unplug events of
 * its queue to sdl_event, which opens and closes the devices there.  The
 * loop's waits also update SDL's joysticks (every millisecond while one is
 * open).  The controller thread then only reads the open devices' state,
 * every kPollMs, drives the motors and watches the Guide buttons and the
 * hotkeys; the devices (the slots) are shared by the two under g_slotMutex.
 *
 * The game's threads only read the state and set the motors, under a mutex
 * or atomics. */
#include "platform/gamepad.h"
#include "platform/joy_names.h"                /* PC (2026-11-23): the names joysticks go by */
#include "platform/log.h"
#include "platform/settings_menu.h"
#include "platform/wheel_ffb.h"                /* PC (2026-11-20): the wheels' force feedback */
#include "platform/system_menu.h"
#include "platform/ui_text.h"                  /* PC (2026-11-16): the interface's languages (N_) */
#include "platform/window.h"

#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/os/win_compat.h"
#endif

#include <atomic>
#include <future>
#include <mutex>
#include <string.h>
#include <thread>

namespace gamepad {
namespace {

constexpr int kPorts = 2;
constexpr int kWheels = 2;
constexpr DWORD kPollMs = 2;
/* The motors: a running rumble is sent again before it ends (SDL rumbles
 * for a duration; the game's motors run until it changes them). */
constexpr Uint32 kRumbleMs = 2000;
constexpr ULONGLONG kRumbleRefreshMs = 1000;
/* A trigger counts as L2 / R2 held from this pressure on. */
constexpr int kTriggerPress = 0x10;
/* The Guide button held this long: the Esc menu (a shorter press: the
 * settings menu, at its release). */
constexpr ULONGLONG kGuideHoldMs = 1500;

/* SDL's buttons on the DualShock 2's (gamepad.h): the State bit and the
 * pressure index (-1 none). */
struct ButtonMap {
    SDL_GamepadButton button;
    int bit;
    int pressure;
};
const ButtonMap kButtons[] = {
    {SDL_GAMEPAD_BUTTON_BACK, 0, -1},           /* Select */
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, 1, -1},     /* L3 */
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 2, -1},    /* R3 */
    {SDL_GAMEPAD_BUTTON_START, 3, -1},          /* Start */
    {SDL_GAMEPAD_BUTTON_DPAD_UP, 4, 2},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 5, 0},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 6, 3},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 7, 1},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 10, 8},  /* L1 */
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 11, 9}, /* R1 */
    {SDL_GAMEPAD_BUTTON_NORTH, 12, 4},          /* Triangle */
    {SDL_GAMEPAD_BUTTON_EAST, 13, 5},           /* Circle */
    {SDL_GAMEPAD_BUTTON_SOUTH, 14, 6},          /* Cross */
    {SDL_GAMEPAD_BUTTON_WEST, 15, 7},           /* Square */
};
struct TriggerMap {
    SDL_GamepadAxis axis;
    int bit;
    int pressure;
};
const TriggerMap kTriggers[] = {
    {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 8, 10},     /* L2 */
    {SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 9, 11},    /* R2 */
};
/* In the answer's order: right x, right y, left x, left y. */
const SDL_GamepadAxis kSticks[4] = {SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFTX,
                                    SDL_GAMEPAD_AXIS_LEFTY};

/* The SDL thread's view of a port. */
struct Slot {
    SDL_Gamepad *pad = nullptr;
    SDL_JoystickID id = 0;
    int sentSmall = -1, sentLarge = -1;     /* the motors last sent (-1: none yet) */
    ULONGLONG sentAt = 0;
    bool guide = false;                     /* the Guide button down */
    bool guideHeld = false;                 /* ... long enough: the Esc menu shown */
    ULONGLONG guideAt = 0;
};
Slot g_slots[kPorts];

/* The SDL thread's view of a wheel (PC 2026-11-20: and its force feedback,
 * when it has one). */
struct WheelSlot {
    SDL_Joystick *joy = nullptr;
    SDL_JoystickID id = 0;
    wheel_ffb::Device ffb;
};
WheelSlot g_wheelSlots[kWheels];
/* PC (2026-11-20): the force feedback GT3 asks for (set_wheel_force /
 * spring), read by the SDL thread; a force not sent again for kForceHoldMs
 * is let go (the game sends it every tick: 17 ms) */
constexpr ULONGLONG kForceHoldMs = 150;
std::mutex g_ffbMutex;
wheel_ffb::Forces g_ffbForces[kWheels];
ULONGLONG g_ffbForceAt[kWheels];
/* PC (2026-11-22): the steering forces sent (set_wheel_force), and the
 * count the SDL thread last played (Forces::sent: the FFB loss workaround) */
unsigned g_ffbForceSeq[kWheels], g_ffbForceSeen[kWheels];
bool g_haptic;                              /* SDL's haptic subsystem started */

/* The SDL thread's view of a device bindings name (input_map::device_name):
 * its joystick, and its gamepad when SDL maps it as one. */
constexpr int kDevices = input_map::kMaxDevices;
struct DeviceSlot {
    SDL_Joystick *joy = nullptr;
    SDL_Gamepad *pad = nullptr;
    SDL_JoystickID id = 0;
};
DeviceSlot g_devSlots[kDevices];
int g_devCount;                             /* the ids opened, 1 .. g_devCount (set before the thread starts) */

/* Shared with the game's threads. */
std::mutex g_mutex;
bool g_present[kPorts];
State g_state[kPorts];
/* PC (2026-11-07): each port's controller's button names (button_names;
 * PC 2026-11-16: English, translated where shown - tr(names.south)) */
const ButtonNames kDefaultNames = {N_("CROSS"), N_("CIRCLE"), N_("SQUARE"), N_("TRIANGLE"), N_("GUIDE")};
ButtonNames g_names[kPorts] = {kDefaultNames, kDefaultNames};
input_map::PadSource g_raw[kPorts];
/* SDL's buttons seen down since the last take_pressed_raw (a tap between
 * two of the game's reads still counts). */
uint32_t g_pressed[kPorts];
std::atomic<int> g_small[kPorts], g_large[kPorts];
bool g_wheelPresent[kWheels];
Joystick g_wheel[kWheels];
bool g_devPresent[kDevices], g_devGamepad[kDevices];
input_map::PadSource g_devPad[kDevices];
Joystick g_devJoy[kDevices];
/* their buttons seen down since the last take_pressed_devices */
uint32_t g_devPressedPad[kDevices];
uint8_t g_devPressedJoy[kDevices][128];

std::thread g_thread;
std::atomic<bool> g_quit{false};
#ifndef _WIN32
/* The slots (g_slots, g_wheelSlots, g_devSlots): opened and closed on the
 * main thread (sdl_event, shutdown), read by the controller thread.  Taken
 * before g_mutex. */
std::mutex g_slotMutex;
std::atomic<bool> g_started{false};         /* init's SDL_InitSubSystem succeeded */
#endif

/* PC: the SDL thread's view of the hotkeys' devices (hotkeys): each one's
 * binding down at the last look; the first look made. */
bool g_hotDown[input_map::kHotkeyCount];
bool g_hotLooked;

/* SDL's buttons and axes of a gamepad as they are. */
void poll_raw(SDL_Gamepad *pad, input_map::PadSource *raw)
{
    raw->buttons = 0;
    for (int b = 0; b < input_map::kPadButtons; b++)
        if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b))
            raw->buttons |= 1u << b;
    for (int a = 0; a < input_map::kPadAxes; a++)
        raw->axes[a] = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)a);
}

void poll(SDL_Gamepad *pad, State *st, input_map::PadSource *raw)
{
    memset(st, 0, sizeof(*st));
    for (const ButtonMap &b : kButtons)
        if (SDL_GetGamepadButton(pad, b.button)) {
            st->buttons |= (uint16_t)(1u << b.bit);
            if (b.pressure >= 0)
                st->pressure[b.pressure] = 0xFF;
        }
    for (const TriggerMap &t : kTriggers) {
        int v = SDL_GetGamepadAxis(pad, t.axis);    /* 0..32767 */
        int p = v <= 0 ? 0 : (v * 255 + 16383) / 32767;
        st->pressure[t.pressure] = (uint8_t)p;
        if (p >= kTriggerPress)
            st->buttons |= (uint16_t)(1u << t.bit);
    }
    for (int i = 0; i < 4; i++)
        st->sticks[i] = (uint8_t)((SDL_GetGamepadAxis(pad, kSticks[i]) + 32768) >> 8);
    poll_raw(pad, raw);
}

/* PC (2026-11-07): a controller's button names (gamepad.h button_names):
 * SDL's labels of its face buttons, the Guide button by its type. */
ButtonNames names_of(SDL_Gamepad *pad)
{
    auto label = [pad](SDL_GamepadButton b, const char *own) -> const char * {
        switch (SDL_GetGamepadButtonLabel(pad, b)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return N_("CROSS");
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return N_("CIRCLE");
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return N_("SQUARE");
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return N_("TRIANGLE");
        default: return own;
        }
    };
    ButtonNames n = kDefaultNames;
    n.south = label(SDL_GAMEPAD_BUTTON_SOUTH, N_("CROSS"));
    n.east = label(SDL_GAMEPAD_BUTTON_EAST, N_("CIRCLE"));
    n.west = label(SDL_GAMEPAD_BUTTON_WEST, N_("SQUARE"));
    n.north = label(SDL_GAMEPAD_BUTTON_NORTH, N_("TRIANGLE"));
    switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        n.guide = "PS";
        break;
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        n.guide = "XBOX";
        break;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        n.guide = N_("HOME");
        break;
    default:
        n.guide = N_("GUIDE");
        break;
    }
    return n;
}

void poll_wheel(SDL_Joystick *joy, Joystick *st)
{
    st->buttonCount = SDL_GetNumJoystickButtons(joy);
    st->axisCount = SDL_GetNumJoystickAxes(joy);
    st->hatCount = SDL_GetNumJoystickHats(joy);
    if (st->buttonCount < 0) st->buttonCount = 0;
    if (st->buttonCount > 128) st->buttonCount = 128;
    if (st->axisCount < 0) st->axisCount = 0;
    if (st->axisCount > 16) st->axisCount = 16;
    if (st->hatCount < 0) st->hatCount = 0;
    if (st->hatCount > 4) st->hatCount = 4;
    for (int i = 0; i < st->buttonCount; i++)
        st->buttons[i] = SDL_GetJoystickButton(joy, i) ? 1 : 0;
    for (int i = 0; i < st->axisCount; i++)
        st->axes[i] = SDL_GetJoystickAxis(joy, i);
    for (int i = 0; i < st->hatCount; i++)
        st->hats[i] = SDL_GetJoystickHat(joy, i);
}

/* The Guide button of a port: a tap (released before kGuideHoldMs) the
 * settings menu, a hold the Esc menu (shown when it is reached). */
void guide(Slot &s, ULONGLONG now)
{
    const bool down = SDL_GetGamepadButton(s.pad, SDL_GAMEPAD_BUTTON_GUIDE);
    if (down && !s.guide) {
        s.guide = true;
        s.guideHeld = false;
        s.guideAt = now;
    } else if (down && !s.guideHeld && now - s.guideAt >= kGuideHoldMs) {
        s.guideHeld = true;
        settings_menu::hide();
        system_menu::toggle();
    } else if (!down && s.guide) {
        s.guide = false;
        if (!s.guideHeld) {
            system_menu::hide();
            settings_menu::toggle();
        }
    }
}

/* PC: the hotkeys' devices (input_map.h [Hotkeys], after `;'): a binding
 * newly down (an axis past half) is a press of its hotkey, which the window
 * thread acts on (window::press_hotkey).  A gamepad's button or axis is read
 * on both ports' controllers, a joystick's on the device it names, else on
 * both wheels.  One held at the first look waits for its release. */
void hotkeys()
{
    using namespace input_map;
    bool any = false;
    for (int h = 0; h < kHotkeyCount; h++)
        any = any || hotkey_device(h).source != Source::none;
    if (!any)
        return;
    bool padOn[kPorts], wheelOn[kWheels];
    PadSource pads[kPorts];
    Joystick wheels[kWheels], devJoy[kDevices];
    bool devOn[kDevices], devPadOn[kDevices];
    PadSource devPad[kDevices];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int p = 0; p < kPorts; p++) {
            padOn[p] = g_present[p];
            pads[p] = g_raw[p];
        }
        for (int w = 0; w < kWheels; w++) {
            wheelOn[w] = g_wheelPresent[w];
            if (wheelOn[w])
                wheels[w] = g_wheel[w];
        }
        for (int d = 0; d < g_devCount; d++) {
            devOn[d] = g_devPresent[d];
            devPadOn[d] = g_devPresent[d] && g_devGamepad[d];
            if (devOn[d]) {
                devJoy[d] = g_devJoy[d];
                devPad[d] = g_devPad[d];
            }
        }
    }
    JoySource wheelSrc[kWheels], devJoySrc[kDevices];
    DeviceSource devSrc[kDevices] = {};
    for (int w = 0; w < kWheels; w++)
        if (wheelOn[w])
            wheelSrc[w] = {wheels[w].buttonCount, wheels[w].axisCount, wheels[w].hatCount, wheels[w].buttons,
                           wheels[w].axes, wheels[w].hats};
    for (int d = 0; d < g_devCount; d++) {
        if (!devOn[d])
            continue;
        const Joystick &j = devJoy[d];
        devJoySrc[d] = {j.buttonCount, j.axisCount, j.hatCount, j.buttons, j.axes, j.hats};
        devSrc[d].joy = &devJoySrc[d];
        devSrc[d].pad = devPadOn[d] ? &devPad[d] : nullptr;
    }
    for (int h = 0; h < kHotkeyCount; h++) {
        const Binding &b = hotkey_device(h);
        if (b.source == Source::none)
            continue;
        float v = 0.0f;
        if (b.device != 0) {
            const Sources s = {nullptr, nullptr, nullptr, 0, devSrc, g_devCount};
            v = value(b, s);
        } else if (b.source == Source::pad_button || b.source == Source::pad_axis) {
            for (int p = 0; p < kPorts; p++)
                if (padOn[p]) {
                    const Sources s = {&pads[p], nullptr, nullptr, 0, nullptr, 0};
                    const float x = value(b, s);
                    v = x > v ? x : v;
                }
        } else {
            for (int w = 0; w < kWheels; w++)
                if (wheelOn[w]) {
                    const Sources s = {nullptr, &wheelSrc[w], nullptr, 0, nullptr, 0};
                    const float x = value(b, s);
                    v = x > v ? x : v;
                }
        }
        const bool down = v >= 0.5f;
        if (down && !g_hotDown[h] && g_hotLooked)
            window::press_hotkey(h);
        g_hotDown[h] = down;
    }
    g_hotLooked = true;
}

void rumble(int port, ULONGLONG now)
{
    Slot &s = g_slots[port];
    int small = g_small[port], large = g_large[port];
    bool on = small != 0 || large != 0;
    if (small == s.sentSmall && large == s.sentLarge && (!on || now - s.sentAt < kRumbleRefreshMs))
        return;
    /* the large motor is the low-frequency one, the small the high */
    SDL_RumbleGamepad(s.pad, (Uint16)(large * 257), small ? 0xFFFF : 0, on ? kRumbleMs : 0);
    s.sentSmall = small;
    s.sentLarge = large;
    s.sentAt = now;
}

/* A wheel's binding names the joystick (`@<name>'): a device of that wheel
 * (its pedals...), never an automatic wheel itself. */
bool named_by_wheel(const char *name)
{
    for (int w = 0; w < kWheels; w++)
        if (input_map::names_device(input_map::profile(input_map::kWheel1 + w), name))
            return true;
    return false;
}

/* The wheel `id' would be: 0 / 1 for Wheel 1 / 2 by its name ([WheelN]
 * Device), else the first free auto one when SDL types it a wheel (and no
 * wheel's binding names it); -1 none (a pad, or no wheel slot wants it).
 * free: only the slots without a joystick count. */
/* PC (2026-11-23): the names are joy_names' (platform/joy_names.h: the 2nd
 * joystick of a name is "<name> #2"), and for an auto wheel -
 *   the joystick its steering is bound to by name is that wheel
 *     (input_map::steer_device: a gt3pc.ini whose wheel bindings all name
 *     the wheel itself);
 *   the lesser part of a device (joy_names::lesser: the collection without
 *     force feedback of a wheel base that shows as two controllers) is none.
 * A user's Fanatec ClubSport DD+ came out as two wheels, the part without
 * force feedback wheel 1. */
int wheel_for(SDL_JoystickID id, bool free)
{
    const std::string unique = joy_names::of(id);
    const char *name = unique.c_str();
    for (int w = 0; w < kWheels; w++) {
        const char *dev = input_map::profile(input_map::kWheel1 + w).device;
        if (dev[0] != '\0' && name[0] != '\0' && _stricmp(dev, name) == 0 &&
            (!free || g_wheelSlots[w].joy == nullptr))
            return w;
    }
    /* an auto wheel's own steering names it */
    bool autoSlot[kWheels];
    for (int w = 0; w < kWheels; w++) {
        const input_map::Profile &p = input_map::profile(input_map::kWheel1 + w);
        const char *steer = input_map::steer_device(p);
        autoSlot[w] = p.device[0] == '\0' && steer[0] == '\0';
        if (p.device[0] == '\0' && steer[0] != '\0' && name[0] != '\0' && _stricmp(steer, name) == 0 &&
            (!free || g_wheelSlots[w].joy == nullptr))
            return w;
    }
    /* (a joystick the bindings name is a wheel's device - pedals, a shifter -
     * unless it has force feedback: the wheel itself, named by its own
     * bindings, joy_names::haptic) */
    if (SDL_GetJoystickTypeForID(id) != SDL_JOYSTICK_TYPE_WHEEL || (named_by_wheel(name) && !joy_names::haptic(id)))
        return -1;
    if (joy_names::lesser(id)) {
        static SDL_JoystickID said;
        if (said != id) {
            said = id;
            pclog::write("wheel: %s is the part without force feedback of a device with one: not taken as a wheel "
                         "(its inputs can be bound by its name)", name);
        }
        return -1;
    }
    for (int w = 0; w < kWheels; w++)
        if (autoSlot[w] && (!free || g_wheelSlots[w].joy == nullptr))
            return w;
    return -1;
}

void plug_wheel(SDL_JoystickID id)
{
    for (int w = 0; w < kWheels; w++)
        if (g_wheelSlots[w].joy != nullptr && g_wheelSlots[w].id == id)
            return;
    const int w = wheel_for(id, true);
    if (w < 0)
        return;
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    if (joy == nullptr) {
        pclog::write("wheel: a wheel could not be opened (%s)", SDL_GetError());
        return;
    }
    WheelSlot &s = g_wheelSlots[w];
    s.joy = joy;
    s.id = id;
    Joystick st;
    poll_wheel(joy, &st);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_wheel[w] = st;
        g_wheelPresent[w] = true;
    }
    const std::string name = joy_names::of(id);        /* (PC 2026-11-23: the name it goes by) */
    pclog::write("wheel: wheel %d: %s (%d axes, %d buttons, %d hats)", w + 1, !name.empty() ? name.c_str() : "a wheel",
                 st.axisCount, st.buttonCount, st.hatCount);
    /* PC (2026-11-20): its force feedback */
    if (!g_haptic)
        pclog::write("wheel: wheel %d: no force feedback (SDL's haptics did not start)", w + 1);
    else if (s.ffb.open(joy))
        pclog::write("wheel: wheel %d: force feedback (%s%s%s)", w + 1, s.ffb.has_constant() ? "the steering force" : "",
                     s.ffb.has_constant() && s.ffb.has_spring() ? ", " : "",
                     s.ffb.has_spring() ? "the centring spring" : "");
    else
        pclog::write("wheel: wheel %d: no force feedback: %s", w + 1, s.ffb.why());
}

void unplug_wheel(SDL_JoystickID id)
{
    for (int w = 0; w < kWheels; w++) {
        WheelSlot &s = g_wheelSlots[w];
        if (s.joy == nullptr || s.id != id)
            continue;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_wheelPresent[w] = false;
        }
        s.ffb.close();                      /* PC (2026-11-20) */
        SDL_CloseJoystick(s.joy);
        s = WheelSlot();
        pclog::write("wheel: wheel %d: disconnected", w + 1);
    }
    /* a wheel left without a slot takes the free one */
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int i = 0; ids != nullptr && i < n; i++)
        if (ids[i] != id)
            plug_wheel(ids[i]);
    SDL_free(ids);
}

/* A joystick a binding names: opened as that device (besides being a pad or
 * a wheel).  PC (2026-11-23): by joy_names' name - the 2nd of a name is
 * "<name> #2" (it was: the first of its name). */
void plug_device(SDL_JoystickID id)
{
    const std::string unique = joy_names::of(id);
    const char *name = unique.c_str();
    if (name[0] == '\0')
        return;
    for (int d = 0; d < g_devCount; d++) {
        DeviceSlot &s = g_devSlots[d];
        if (s.joy != nullptr || _stricmp(input_map::device_name(d + 1), name) != 0)
            continue;                       /* (taken: by this one, or another of its name) */
        SDL_Joystick *joy = SDL_OpenJoystick(id);
        if (joy == nullptr) {
            pclog::write("pad: device %d (%s) could not be opened (%s)", d + 1, name, SDL_GetError());
            return;
        }
        s.joy = joy;
        s.id = id;
        s.pad = SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : nullptr;
        Joystick st;
        input_map::PadSource raw = {};
        poll_wheel(joy, &st);
        if (s.pad != nullptr)
            poll_raw(s.pad, &raw);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_devJoy[d] = st;
            g_devPad[d] = raw;
            g_devGamepad[d] = s.pad != nullptr;
            g_devPressedPad[d] = 0;
            memset(g_devPressedJoy[d], 0, sizeof(g_devPressedJoy[d]));
            g_devPresent[d] = true;
        }
        pclog::write("pad: device %d: %s (%d axes, %d buttons, %d hats%s)", d + 1, name, st.axisCount, st.buttonCount,
                     st.hatCount, s.pad != nullptr ? "; a gamepad" : "");
        return;
    }
}

void unplug_device(SDL_JoystickID id)
{
    for (int d = 0; d < g_devCount; d++) {
        DeviceSlot &s = g_devSlots[d];
        if (s.joy == nullptr || s.id != id)
            continue;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_devPresent[d] = false;
        }
        if (s.pad != nullptr)
            SDL_CloseGamepad(s.pad);
        SDL_CloseJoystick(s.joy);
        s = DeviceSlot();
        pclog::write("pad: device %d: %s disconnected", d + 1, input_map::device_name(d + 1));
        /* another of its name takes its place */
        int n = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks(&n);
        for (int i = 0; ids != nullptr && i < n; i++)
            if (ids[i] != id)
                plug_device(ids[i]);
        SDL_free(ids);
    }
}

void plug(SDL_JoystickID id)
{
    if (wheel_for(id, false) >= 0)
        return;                             /* a wheel (plug_wheel) */
    int port = -1;
    for (int p = 0; p < kPorts; p++) {
        if (g_slots[p].pad != nullptr && g_slots[p].id == id)
            return;
        if (g_slots[p].pad == nullptr && port < 0)
            port = p;
    }
    if (port < 0) {
        pclog::write("pad: a controller was connected, but both ports are in use");
        return;
    }
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (pad == nullptr) {
        pclog::write("pad: a controller could not be opened (%s)", SDL_GetError());
        return;
    }
    Slot &s = g_slots[port];
    s = Slot();
    s.pad = pad;
    s.id = id;
    s.guide = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE);
    s.guideHeld = true;                     /* (held while plugged in: not a tap) */
    SDL_SetGamepadPlayerIndex(pad, port);   /* the DualShock 4's light bar colour of player 1 / 2 */
    const ButtonNames names = names_of(pad);    /* PC (2026-11-07) */
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        poll(pad, &g_state[port], &g_raw[port]);
        g_names[port] = names;
        g_present[port] = true;
    }
    const char *name = SDL_GetGamepadName(pad);
    pclog::write("pad: port %d: %s (its buttons %s, %s, %s, %s, %s)", port + 1, name ? name : "a controller",
                 names.south, names.east, names.west, names.north, names.guide);
}

void unplug(SDL_JoystickID id)
{
    for (int p = 0; p < kPorts; p++) {
        Slot &s = g_slots[p];
        if (s.pad == nullptr || s.id != id)
            continue;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_present[p] = false;
        }
        /* the next controller in this port starts still (the game sets its
         * motors again once the port is set up) */
        g_small[p] = 0;
        g_large[p] = 0;
        SDL_CloseGamepad(s.pad);
        s = Slot();
        pclog::write("pad: port %d: disconnected", p + 1);
    }
    /* a controller left without a port takes the free one */
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids != nullptr && i < n; i++)
        if (ids[i] != id)
            plug(ids[i]);
    SDL_free(ids);
}

/* A plug / unplug event of SDL's queue. */
void device_event(const SDL_Event &e)
{
    if (e.type == SDL_EVENT_GAMEPAD_ADDED)
        plug(e.gdevice.which);
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
        unplug(e.gdevice.which);
    else if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
        plug_wheel(e.jdevice.which);
        plug_device(e.jdevice.which);
    } else if (e.type == SDL_EVENT_JOYSTICK_REMOVED) {
        unplug_wheel(e.jdevice.which);
        unplug_device(e.jdevice.which);
        joy_names::forget(e.jdevice.which);     /* PC (2026-11-23) */
    }
}

/* Reads each open pad, wheel and device into the shared state, drives the
 * motors and the wheels' forces and watches the Guide buttons. */
void poll_devices(ULONGLONG now)
{
    for (int p = 0; p < kPorts; p++) {
        if (g_slots[p].pad == nullptr)
            continue;
        State st;
        input_map::PadSource raw;
        poll(g_slots[p].pad, &st, &raw);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_state[p] = st;
            g_raw[p] = raw;
            g_pressed[p] |= raw.buttons;
        }
        rumble(p, now);
        guide(g_slots[p], now);
    }
    for (int w = 0; w < kWheels; w++) {
        if (g_wheelSlots[w].joy == nullptr)
            continue;
        Joystick st;
        poll_wheel(g_wheelSlots[w].joy, &st);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_wheel[w] = st;
        }
        /* PC (2026-11-20): the forces the game asks for, with the
         * settings as they are now (the in-game menu changes them) */
        if (g_wheelSlots[w].ffb.is_open()) {
            wheel_ffb::Forces f;
            {
                std::lock_guard<std::mutex> lock(g_ffbMutex);
                f = g_ffbForces[w];
                f.sent = g_ffbForceSeq[w] != g_ffbForceSeen[w];     /* (PC 2026-11-22) */
                g_ffbForceSeen[w] = g_ffbForceSeq[w];
                if (now - g_ffbForceAt[w] > kForceHoldMs)
                    f.force = 0.0f;     /* (not sent again: the game paused or stalled) */
            }
            wheel_ffb::Settings set;
            set.strength = input_map::ffb(w, input_map::kFfbStrength);
            set.spring = input_map::ffb(w, input_map::kFfbSpring);
            set.minimum = input_map::ffb(w, input_map::kFfbMinimum);
            set.invert = input_map::ffb(w, input_map::kFfbInvert);
            set.restart = input_map::ffb(w, input_map::kFfbLossWorkaround);  /* PC (2026-11-22) */
            g_wheelSlots[w].ffb.apply(f, set);
        }
    }
    for (int d = 0; d < g_devCount; d++) {
        const DeviceSlot &s = g_devSlots[d];
        if (s.joy == nullptr)
            continue;
        Joystick st;
        input_map::PadSource raw = {};
        poll_wheel(s.joy, &st);
        if (s.pad != nullptr)
            poll_raw(s.pad, &raw);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_devJoy[d] = st;
        g_devPad[d] = raw;
        g_devPressedPad[d] |= raw.buttons;
        for (int i = 0; i < st.buttonCount; i++)
            g_devPressedJoy[d][i] |= st.buttons[i];
    }
}

/* Stops the motors and closes every open device. */
void close_devices()
{
    for (int p = 0; p < kPorts; p++)
        if (g_slots[p].pad != nullptr) {
            SDL_RumbleGamepad(g_slots[p].pad, 0, 0, 0);
            SDL_CloseGamepad(g_slots[p].pad);
            g_slots[p] = Slot();
        }
    for (int w = 0; w < kWheels; w++)
        if (g_wheelSlots[w].joy != nullptr) {
            g_wheelSlots[w].ffb.close();    /* PC (2026-11-20): the forces stopped */
            SDL_CloseJoystick(g_wheelSlots[w].joy);
            g_wheelSlots[w] = WheelSlot();
        }
    for (int d = 0; d < g_devCount; d++)
        if (g_devSlots[d].joy != nullptr) {
            if (g_devSlots[d].pad != nullptr)
                SDL_CloseGamepad(g_devSlots[d].pad);
            SDL_CloseJoystick(g_devSlots[d].joy);
            g_devSlots[d] = DeviceSlot();
        }
}

#ifdef _WIN32
void thread_main(std::promise<bool> *started)
{
    /* no SDL window: the controllers count whatever window has the focus */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        pclog::write("pad: SDL3 did not start (%s): keyboard only", SDL_GetError());
        started->set_value(false);
        return;
    }
    /* PC (2026-11-20): the wheels' force feedback (wheel_ffb.h) */
    g_haptic = SDL_InitSubSystem(SDL_INIT_HAPTIC);
    if (!g_haptic)
        pclog::write("wheel: SDL's haptics did not start (%s): no force feedback", SDL_GetError());
    started->set_value(true);

    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            device_event(e);
        poll_devices(GetTickCount64());
        hotkeys();                          /* PC */
        Sleep(kPollMs);
    }

    close_devices();
    SDL_Quit();
}
#else
/* The controller thread: the open devices' state only (the main thread
 * opens and closes them). */
void thread_main()
{
    while (!g_quit) {
        {
            std::lock_guard<std::mutex> lock(g_slotMutex);
            poll_devices(GetTickCount64());
        }
        hotkeys();                          /* PC */
        Sleep(kPollMs);
    }
}
#endif

} // namespace

#ifdef _WIN32
bool init()
{
    /* SDL3.dll is delay-loaded: loaded here from the game's folder (not the
     * working folder), no SDL function is called without it */
    HMODULE dll = LoadLibraryExW(L"SDL3.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dll == nullptr) {
        pclog::write("pad: SDL3.dll not found next to the game: keyboard only");
        return false;
    }
    int v = SDL_GetVersion();
    if (SDL_VERSIONNUM_MAJOR(v) != 3 || SDL_VERSIONNUM_MINOR(v) < SDL_MINOR_VERSION) {
        pclog::write("pad: SDL3.dll is version %d.%d.%d, %d.%d or later is needed: keyboard only",
                     SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), SDL_MAJOR_VERSION,
                     SDL_MINOR_VERSION);
        return false;
    }
    /* the devices the bindings name (input_map::init has read them) */
    g_devCount = input_map::device_count() < kDevices ? input_map::device_count() : kDevices;
    for (int d = 0; d < input_map::device_count(); d++)
        pclog::write("pad: device %d named by the bindings: %s%s", d + 1, input_map::device_name(d + 1),
                     d < kDevices ? "" : " (too many: not read)");
    std::promise<bool> started;
    std::future<bool> ok = started.get_future();
    g_quit = false;
    g_thread = std::thread(thread_main, &started);
    if (!ok.get()) {
        g_thread.join();
        return false;
    }
    pclog::write("pad: SDL %d.%d.%d: controllers on ports 1 and 2, wheels 1 and 2", SDL_VERSIONNUM_MAJOR(v),
                 SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    return true;
}

void shutdown()
{
    if (!g_thread.joinable())
        return;
    g_quit = true;
    g_thread.join();
}
#else
bool init()
{
    const int v = SDL_GetVersion();
    /* the devices the bindings name (input_map::init has read them) */
    g_devCount = input_map::device_count() < kDevices ? input_map::device_count() : kDevices;
    for (int d = 0; d < input_map::device_count(); d++)
        pclog::write("pad: device %d named by the bindings: %s%s", d + 1, input_map::device_name(d + 1),
                     d < kDevices ? "" : " (too many: not read)");
    /* the controllers count while the window is in the background too */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        pclog::write("pad: SDL3 did not start (%s): keyboard only", SDL_GetError());
        return false;
    }
    /* the wheels' force feedback (wheel_ffb.h) */
    g_haptic = SDL_InitSubSystem(SDL_INIT_HAPTIC);
    if (!g_haptic)
        pclog::write("wheel: SDL's haptics did not start (%s): no force feedback", SDL_GetError());
    g_started = true;
    g_quit = false;
    g_thread = std::thread(thread_main);
    pclog::write("pad: SDL %d.%d.%d: controllers on ports 1 and 2, wheels 1 and 2", SDL_VERSIONNUM_MAJOR(v),
                 SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    return true;
}

void shutdown()
{
    if (!g_thread.joinable())
        return;
    g_quit = true;
    g_thread.join();
    {
        std::lock_guard<std::mutex> lock(g_slotMutex);
        close_devices();
        g_started = false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int p = 0; p < kPorts; p++)
            g_present[p] = false;
        for (int w = 0; w < kWheels; w++)
            g_wheelPresent[w] = false;
        for (int d = 0; d < kDevices; d++)
            g_devPresent[d] = false;
    }
    if (g_haptic) {
        SDL_QuitSubSystem(SDL_INIT_HAPTIC);
        g_haptic = false;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void sdl_event(const SDL_Event &e)
{
    if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED &&
        e.type != SDL_EVENT_JOYSTICK_ADDED && e.type != SDL_EVENT_JOYSTICK_REMOVED)
        return;
    std::lock_guard<std::mutex> lock(g_slotMutex);
    if (g_started)
        device_event(e);
}
#endif

bool read(int port, State *out)
{
    if (port < 0 || port >= kPorts)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_present[port])
        return false;
    *out = g_state[port];
    return true;
}

bool read_raw(int port, input_map::PadSource *out)
{
    if (port < 0 || port >= kPorts)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_present[port])
        return false;
    *out = g_raw[port];
    return true;
}

uint32_t take_pressed_raw(int port)
{
    if (port < 0 || port >= kPorts)
        return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t b = g_present[port] ? g_pressed[port] : 0;
    g_pressed[port] = 0;
    return b;
}

void set_motors(int port, int small, int large)
{
    if (port < 0 || port >= kPorts)
        return;
    g_small[port] = small != 0 ? 1 : 0;
    g_large[port] = large < 0 ? 0 : large > 255 ? 255 : large;
}

/* PC (2026-11-07) */
ButtonNames button_names()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int p = 0; p < kPorts; p++)
        if (g_present[p])
            return g_names[p];
    return kDefaultNames;
}

bool read_wheel(int wheel, Joystick *out)
{
    if (wheel < 0 || wheel >= kWheels)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_wheelPresent[wheel])
        return false;
    *out = g_wheel[wheel];
    return true;
}

/* PC (2026-11-20) */
void set_wheel_force(int wheel, float force)
{
    if (wheel < 0 || wheel >= kWheels)
        return;
    std::lock_guard<std::mutex> lock(g_ffbMutex);
    g_ffbForces[wheel].force = force < -1.0f ? -1.0f : force > 1.0f ? 1.0f : force;
    g_ffbForceAt[wheel] = GetTickCount64();
    g_ffbForceSeq[wheel]++;                 /* PC (2026-11-22): one more sent */
}

void set_wheel_spring(int wheel, bool on, float k, float clip)
{
    if (wheel < 0 || wheel >= kWheels)
        return;
    std::lock_guard<std::mutex> lock(g_ffbMutex);
    g_ffbForces[wheel].spring = on;
    g_ffbForces[wheel].springK = k;
    g_ffbForces[wheel].springClip = clip;
}

void read_devices(Devices *out)
{
    out->count = g_devCount;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int d = 0; d < out->count; d++) {
            out->present[d] = g_devPresent[d];
            out->gamepad[d] = g_devPresent[d] && g_devGamepad[d];
            if (out->present[d]) {
                out->joy[d] = g_devJoy[d];
                out->pad[d] = g_devPad[d];
            }
        }
    }
    for (int d = 0; d < out->count; d++) {
        out->src[d] = {};
        if (!out->present[d])
            continue;
        const Joystick &j = out->joy[d];
        out->joySrc[d] = {j.buttonCount, j.axisCount, j.hatCount, j.buttons, j.axes, j.hats};
        out->src[d].pad = out->gamepad[d] ? &out->pad[d] : nullptr;
        out->src[d].joy = &out->joySrc[d];
    }
}

bool device_present(int id)
{
    if (id < 1 || id > g_devCount)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_devPresent[id - 1];
}

void take_pressed_devices(DevicePresses *out)
{
    memset(out, 0, sizeof(*out));
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int d = 0; d < g_devCount; d++) {
        if (g_devPresent[d]) {
            out->pad[d] = g_devPressedPad[d];
            memcpy(out->joy[d], g_devPressedJoy[d], sizeof(out->joy[d]));
        }
        g_devPressedPad[d] = 0;
        memset(g_devPressedJoy[d], 0, sizeof(g_devPressedJoy[d]));
    }
}

} // namespace gamepad
