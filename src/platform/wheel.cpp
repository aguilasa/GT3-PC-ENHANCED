/* wheel.cpp - the USB steering wheels of GT3 PC (wheel.h). */
#include "platform/wheel.h"
#include "platform/cockpit_osd.h"              /* PC (2026-11-08): the cockpit camera's calibration */
#include "platform/debug_camera.h"
#include "platform/gamepad.h"
#include "platform/input_map.h"
#include "platform/settings_menu.h"
#include "platform/system_menu.h"
#include "platform/window.h"

#include <atomic>
#include <math.h>
#include <string.h>

namespace wheel {
namespace {

using namespace input_map;

float control_value(const Profile &p, int c, const Sources &s)
{
    float v = 0.0f;
    for (int slot = 0; slot < kSlots; slot++) {
        const float x = value(p.map[c][slot], s);
        if (x > v)
            v = x;
    }
    return v;
}

/* the mouse wheel's notches (window::mouse_wheel) since the last read */
uint32_t g_wheelUp[2], g_wheelDown[2];

} // namespace

bool read(int port, State *out)
{
    gamepad::Joystick joy;
    if (port < 0 || port > 1 || !gamepad::read_wheel(port, &joy))
        return false;
    memset(out, 0, sizeof(*out));
    out->x = 0x200;
    out->hat = 8;
    if (settings_menu::shown() || system_menu::shown() || debug_camera::holds_input())
        return true;                        /* the menus (or the debug camera, unlocked) have the input: at rest */
    const int prof = kWheel1 + port;
    const Profile &p = profile(prof);
    const JoySource js = {joy.buttonCount, joy.axisCount, joy.hatCount, joy.buttons, joy.axes, joy.hats};
    uint32_t up, down;
    window::mouse_wheel(&up, &down);
    const int notches = (up != g_wheelUp[port] ? 1 : 0) | (down != g_wheelDown[port] ? 2 : 0);
    g_wheelUp[port] = up;
    g_wheelDown[port] = down;
    gamepad::Devices devs;                  /* (pedals of their own, a shifter...: `@<name>') */
    gamepad::read_devices(&devs);
    const Sources s = {nullptr, &js, window::key_down, notches, devs.src, devs.count};

    /* the steering: right - left, its dead zone about the centre */
    float steer = control_value(p, WHEEL_STEER_RIGHT, s) - control_value(p, WHEEL_STEER_LEFT, s);
    const float mag = apply_deadzone(fabsf(steer), deadzone(prof, 0) / 100.0f);
    steer = steer < 0.0f ? -mag : mag;
    out->x = 0x200 + (int)lroundf(steer * 0x1FF);
    out->gas = (int)lroundf(apply_deadzone(control_value(p, WHEEL_GAS, s), deadzone(prof, 1) / 100.0f) * 255.0f);
    out->brake = (int)lroundf(apply_deadzone(control_value(p, WHEEL_BRAKE, s), deadzone(prof, 2) / 100.0f) * 255.0f);
    for (int i = 0; i < kWheelButtons; i++)
        out->buttons[i] = control_value(p, WHEEL_CROSS + i, s) >= 0.5f ? 1 : 0;
    /* the D-pad as a hat: 0 up, clockwise */
    const bool u = control_value(p, WHEEL_UP, s) >= 0.5f, d = control_value(p, WHEEL_DOWN, s) >= 0.5f;
    const bool l = control_value(p, WHEEL_LEFT, s) >= 0.5f, r = control_value(p, WHEEL_RIGHT, s) >= 0.5f;
    const int dy = (d ? 1 : 0) - (u ? 1 : 0), dx = (r ? 1 : 0) - (l ? 1 : 0);
    static const int kHat[3][3] = {{7, 0, 1}, {6, 8, 2}, {5, 4, 3}};    /* [dy + 1][dx + 1] */
    out->hat = kHat[dy + 1][dx + 1];
    /* PC (2026-11-08): the cockpit camera's calibration (cockpit_osd.h) has
     * the calibrating player's D-pad and Cross / Circle / Triangle while it
     * is on (the other player's wheel takes it over with a press): their
     * state to it, and - when they are its - released for the game */
    if (cockpit_osd::active()) {
        const unsigned held = (u ? cockpit_osd::kPadUp : 0) | (d ? cockpit_osd::kPadDown : 0) |
                              (l ? cockpit_osd::kPadLeft : 0) | (r ? cockpit_osd::kPadRight : 0) |
                              (out->buttons[WHEEL_CROSS - WHEEL_CROSS] ? cockpit_osd::kPadCross : 0) |
                              (out->buttons[WHEEL_CIRCLE - WHEEL_CROSS] ? cockpit_osd::kPadCircle : 0) |
                              (out->buttons[WHEEL_TRIANGLE - WHEEL_CROSS] ? cockpit_osd::kPadTriangle : 0);
        if (cockpit_osd::pad_state(1, port, held)) {
            out->hat = 8;
            out->buttons[WHEEL_CROSS - WHEEL_CROSS] = 0;
            out->buttons[WHEEL_CIRCLE - WHEEL_CROSS] = 0;
            out->buttons[WHEEL_TRIANGLE - WHEEL_CROSS] = 0;
        }
    }
    return true;
}

bool connected(int port)
{
    gamepad::Joystick joy;
    return port >= 0 && port <= 1 && gamepad::read_wheel(port, &joy);
}

bool preferred(int port)
{
    return !plays_with_controller(port);
}

/* PC (2026-11-20) */
void force(int port, float f)
{
    gamepad::set_wheel_force(port, f);
}

void spring(int port, bool on, float k, float clip)
{
    gamepad::set_wheel_spring(port, on, k, clip);
}

/* PC (2026-11-20) */
static std::atomic<bool> g_gameFfb[2] = {{true}, {true}};

void set_game_ffb(int port, bool on)
{
    if (port >= 0 && port <= 1)
        g_gameFfb[port].store(on, std::memory_order_relaxed);
}

bool game_ffb(int port)
{
    return port < 0 || port > 1 || g_gameFfb[port].load(std::memory_order_relaxed);
}

} // namespace wheel
