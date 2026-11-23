/* gamepad.h - game controllers and wheels of GT3 PC, through SDL3.
 *
 * A DualShock 4 (or any controller SDL3 knows as a gamepad: DualSense,
 * Xbox, Switch Pro...) stands for a DualShock 2: the first one connected
 * plugs into port 1, the second into port 2; unplugged, its port is free for
 * the next.  A wheel (a joystick SDL3 types as a wheel, or the one named by
 * gt3pc.ini [Wheel1] / [Wheel2] Device) is Wheel 1 / 2 instead, never a
 * pad.  A joystick a binding names (input_map.h `@<name>': a wheel's pedals
 * of their own, a shifter, a button box) is opened besides, the first of
 * that name, for the bindings that read it; it stays a pad or a wheel too
 * if it is one.  SDL3.dll is loaded at start-up when it is next to the game (it is
 * delay-loaded); without it the game runs with the keyboard alone.  A
 * thread of its own polls SDL (every 2 ms), so the pads read a recent state
 * at any time; the controllers work while the window is in the background,
 * as in PCSX2.  Off Windows SDL3 is linked in and runs on the main thread
 * (the window's): the window's loop hands the plug / unplug events to
 * sdl_event, and the thread only reads the open devices' state.
 * platform/pad_pc.cpp maps the raw state to libpad's answers and
 * platform/wheel.cpp the wheels' to the USB wheel driver's, both through
 * the mappings of platform/input_map.h.
 *
 * The Guide button (PS / Xbox / Home) of any controller is the menus': a
 * tap shows or hides the settings menu (F2), a hold of 1.5 s the Esc menu.
 * PC: a hotkey's device binding (input_map.h [Hotkeys], after `;') pressed
 * is that hotkey (window::press_hotkey).
 *
 * The DualShock 2 of State (the menus read it: the physical buttons, not
 * the mapping): the buttons by position (Cross = the bottom face button),
 * D-pad, Start = Options / Menu, Select = Share / View, L3 / R3 the stick
 * clicks; L2 / R2 from the triggers.  The two motors of the game drive the
 * controller's rumble (the large one the low-frequency motor). */
#pragma once

#include <stdint.h>

#include "platform/input_map.h"

#ifndef _WIN32
union SDL_Event;
#endif

namespace gamepad {

/* A DualShock 2's state.  buttons: the answer's bytes 2..3 (libpad), active
 * high: bit 0..7 = Select, L3, R3, Start, Up, Right, Down, Left; bit 8..15 =
 * L2, R2, L1, R1, Triangle, Circle, Cross, Square.  sticks: right x, right
 * y, left x, left y (0x80 centred, 0 = left / up).  pressure: Right, Left,
 * Up, Down, Triangle, Circle, Cross, Square, L1, R1, L2, R2. */
struct State {
    uint16_t buttons;
    uint8_t sticks[4];
    uint8_t pressure[12];
};

/* Loads SDL3 and starts the controller thread; false (logged) when SDL3.dll
 * is missing or does not start.  Off Windows: on the main thread, after
 * window::create (SDL's controllers started there). */
bool init();
/* Stops the motors and the thread. */
void shutdown();

/* The state of the controller in `port` (0, 1); false when none. */
bool read(int port, State *out);
/* The raw state of the controller in `port' (SDL's buttons and axes as they
 * are); false when none. */
bool read_raw(int port, input_map::PadSource *out);
/* SDL's buttons (input_map::PadSource bits) seen down since the last call:
 * a tap between two of the game's reads (platform/pad_pc.h). */
uint32_t take_pressed_raw(int port);

/* The game's motors for `port`: small on (0 / 1), large 0..255. */
void set_motors(int port, int small, int large);

/* PC (2026-11-07): the controller's button names as its maker prints them
 * (SDL's labels), for the overlays' hints (the settings menu, the Esc menu,
 * the song changer): the four face buttons by position - south, east, west,
 * north: CROSS, CIRCLE, SQUARE, TRIANGLE on a PlayStation controller; A, B,
 * X, Y on an Xbox one; B, A, Y, X on a Nintendo one - and the Guide button
 * (PS, XBOX, HOME; GUIDE else).  The controller in port 1's, else port 2's;
 * a DualShock's (GUIDE) while none is connected.  Capitals.  Any thread. */
struct ButtonNames {
    const char *south, *east, *west, *north, *guide;
};
ButtonNames button_names();

/* A wheel's joystick as SDL reads it. */
struct Joystick {
    int buttonCount, axisCount, hatCount;
    uint8_t buttons[128];
    int16_t axes[16];
    uint8_t hats[4];                        /* SDL_HAT_* bits */
};
/* The joystick of Wheel 1 / 2 (`wheel' 0, 1); false when none. */
bool read_wheel(int wheel, Joystick *out);

/* The devices bindings name (input_map::device_name; ids 1 .. count, at
 * most input_map::kMaxDevices), as read.  src: the bindings' view of them
 * (input_map::Sources::devices), pointing into this struct: read in place,
 * not copied. */
struct Devices {
    int count;
    bool present[input_map::kMaxDevices];
    bool gamepad[input_map::kMaxDevices];   /* SDL maps it as a gamepad: pad[] */
    input_map::PadSource pad[input_map::kMaxDevices];
    Joystick joy[input_map::kMaxDevices];
    input_map::JoySource joySrc[input_map::kMaxDevices];
    input_map::DeviceSource src[input_map::kMaxDevices];
};
void read_devices(Devices *out);
/* Device `id' is connected. */
bool device_present(int id);
/* Their buttons seen down since the last call (a tap between two of the
 * game's reads): [id - 1], pad SDL's gamepad buttons (bits), joy the
 * joystick's. */
struct DevicePresses {
    uint32_t pad[input_map::kMaxDevices];
    uint8_t joy[input_map::kMaxDevices][128];
};
void take_pressed_devices(DevicePresses *out);

#ifndef _WIN32
/* Off Windows: an event of SDL's queue, from the window's loop (the main
 * thread); a controller's or joystick's plug / unplug opens or closes it,
 * the other events are ignored. */
void sdl_event(const SDL_Event &e);
#endif

} // namespace gamepad
