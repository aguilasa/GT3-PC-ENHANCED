/* clutch_input.cpp - the players' clutches and H-shifters (clutch_input.h). */
#include "platform/clutch_input.h"
#include "platform/debug_camera.h"
#include "platform/gamepad.h"
#include "platform/input_map.h"
#include "platform/settings_menu.h"
#include "platform/system_menu.h"
#include "platform/window.h"

#include <mutex>
#include <stdio.h>
#include <string.h>

namespace clutch_input {
namespace {

using namespace input_map;

bool menus_hold()
{
    return settings_menu::shown() || system_menu::shown() || debug_camera::holds_input();
}

bool maps(const Profile &p, int c)
{
    for (int s = 0; s < kSlots; s++)
        if (p.map[c][s].source != Source::none)
            return true;
    return false;
}

/* Each player's gate (gate()): its value, and whether the last gate held
 * was a key / mouse button's (kept once released) */
struct Gate {
    int gate = 0;
    bool latched = false;
};
std::mutex g_lock;
Gate g_gates[2];

} // namespace

float clutch(int player, bool wheel)
{
    if (player < 0 || player > 1)
        return -1.0f;
    const int prof = (wheel ? kWheel1 : kController1) + player;
    const int c = clutch_control(prof);
    const Profile &p = profile(prof);
    if (c < 0 || !maps(p, c))
        return -1.0f;
    if (menus_hold())
        return 0.0f;
    PadSource raw;
    gamepad::Joystick joy;
    JoySource js = {};
    const bool havePad = !wheel && gamepad::read_raw(player, &raw);
    const bool haveJoy = wheel && gamepad::read_wheel(player, &joy);
    if (haveJoy)
        js = JoySource{joy.buttonCount, joy.axisCount, joy.hatCount, joy.buttons, joy.axes, joy.hats};
    gamepad::Devices devs;
    gamepad::read_devices(&devs);
    const Sources s = {havePad ? &raw : nullptr, haveJoy ? &js : nullptr, window::key_down, 0, devs.src, devs.count};
    float v = 0.0f;
    for (int slot = 0; slot < kSlots; slot++) {
        const float x = value(p.map[c][slot], s);
        v = x > v ? x : v;
    }
    /* PC (2026-11-10): a wheel's clutch pedal dead zone (ClutchDeadzone, at
     * the start of its travel as the gas and brake pedals') */
    if (wheel)
        return apply_deadzone(v, deadzone(prof, kWheelClutchDeadzone) / 100.0f);
    return v > 1.0f ? 1.0f : v;
}

int gate(int player)
{
    if (player < 0 || player > 1)
        return -1;
    const Profile &p = profile(kHShifter1 + player);
    bool any = false;
    for (int c = 0; c < kHShifterControlCount && !any; c++)
        any = maps(p, c);
    if (!any)
        return -1;
    std::lock_guard<std::mutex> hold(g_lock);
    Gate &g = g_gates[player];
    if (menus_hold())
        return g.gate;
    gamepad::Devices devs;
    gamepad::read_devices(&devs);
    const Sources s = {nullptr, nullptr, window::key_down, 0, devs.src, devs.count};
    int held = -1;
    bool heldKey = false;
    for (int c = 0; c < kHShifterControlCount; c++) {
        const Binding &b = p.map[c][0];
        if (b.source == Source::none || value(b, s) < 0.5f)
            continue;
        held = c;                           /* (the last gate held; HS_NEUTRAL = 0, HS_GEARn = n, HS_REVERSE = 10) */
        heldKey = b.source == Source::key || b.source == Source::mouse_wheel;
    }
    if (held >= 0) {
        g.gate = held;
        g.latched = heldKey;
    } else if (!g.latched) {
        g.gate = 0;                         /* (a shifter's lever out of its gate) */
    }
    return g.gate;
}

/* PC (2026-11-23) */
bool gate_held(int player)
{
    if (player < 0 || player > 1 || menus_hold())
        return false;
    const Profile &p = profile(kHShifter1 + player);
    bool any = false;
    for (int c = 0; c < kHShifterControlCount && !any; c++)
        any = maps(p, c);
    if (!any)
        return false;
    gamepad::Devices devs;
    gamepad::read_devices(&devs);
    const Sources s = {nullptr, nullptr, window::key_down, 0, devs.src, devs.count};
    for (int c = 0; c < kHShifterControlCount; c++) {
        const Binding &b = p.map[c][0];
        if (b.source != Source::none && value(b, s) >= 0.5f)
            return true;
    }
    return false;
}

void clutch_text(int profile, char *out, int n)
{
    if (n <= 0)
        return;
    out[0] = '\0';
    const int c = clutch_control(profile);
    if (c < 0)
        return;
    const Profile &p = input_map::profile(profile);
    for (int s = 0; s < kSlots; s++) {
        char d[256];                        /* (PC 2026-11-16: 160 before - describe's UTF-8 translations) */
        describe(p.map[c][s], d, sizeof(d));
        if (d[0] == '\0')
            continue;
        const size_t len = strlen(out);
        snprintf(out + len, (size_t)n - len, "%s%s", len > 0 ? " / " : "", d);
    }
}

} // namespace clutch_input
