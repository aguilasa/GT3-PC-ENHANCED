/* wheel_ffb.h - PC (2026-11-20, user: "Force feedback for the virtual wheels,
 * currently none is implemented.  I want: FFB Tester(s) in the launcher,
 * button sends a brief FFB signal.  FFB settings: Strength, etc."): a wheel's
 * force feedback through SDL3's haptics, shared by the game (platform/
 * gamepad.cpp: its SDL thread plays the forces GT3 sends its Driving Force,
 * engine/input/usb_pc.cpp) and the launcher (launcher/controls.cpp: the
 * Controls tab's testers) - one implementation, so the testers play what
 * the game would.  A Device belongs to the thread that opened its joystick.
 *
 * The forces, in the game's terms: the steering force -1..1, + pushing the
 * wheel to the right (GT3's constant force byte 0x80 - force x 127: below
 * 0x80 to the right - its own centring sequence, T479 autoCentre, turns a
 * wheel right of centre with the bytes above it), and the centring spring
 * (the Logitech protocol's Set Default Spring: K 0..7, clip 0..255; GT3
 * sets K 3 and clip 0x1E, on in the menus and off in a race).  The settings
 * (input_map::FfbSetting, [WheelN] Ffb*): the strength scales the steering
 * force, the minimum lifts a non-zero one to at least that much, invert
 * turns both ways round; the centring spring setting scales the spring. */
#pragma once

struct SDL_Joystick;
struct SDL_Haptic;

namespace wheel_ffb {

/* input_map::Profile::ffb, as percents (invert 0 / 1).  PC (2026-11-22):
 * restart 1 (FfbLossWorkaround on) - a user's Simagic Alpha Evo lost its
 * forces some 30 - 40 s into a race, which PCSX2 works round with its
 * "Workaround for Intermittent FFB Loss" (FfbDropoutWorkaround).  The same
 * as PCSX2's (its usb-pad-sdl-ff.cpp SetConstantForce, read 2026-11-22):
 * the steering force started again with each one the game sends (GT3: every
 * tick), changed or not; the spring left alone.  PCSX2's notes: some wheels
 * (the Simagic Alpha Mini) do not keep an effect of infinite length going;
 * restarting a playing effect costs most wheels detail and makes some cut
 * out at each update - so it is off by default. */
struct Settings {
    int strength = 100, spring = 100, minimum = 0, invert = 0, restart = 0;
};

/* What the game asks of a wheel. */
struct Forces {
    float force = 0.0f;                     /* the steering force, -1..1, + to the right */
    /* PC (2026-11-22): the game sent a steering force since the last apply
     * (PCSX2's SetConstantForce call; Settings::restart starts it again) */
    bool sent = false;
    bool spring = false;                    /* the centring spring on */
    float springK = 0.0f;                   /* its coefficient, 0..1 (the protocol's K / 7) */
    float springClip = 0.0f;                /* its most force, 0..1 (clip / 255) */
};

/* The steering force as sent: x the strength, a non-zero one lifted to the
 * minimum, inverted, held to -1..1. */
float scaled(float force, const Settings &s);

class Device {
public:
    /* The joystick's force feedback opened (the device's own centring and
     * gain left to the forces: autocenter 0, gain 100); false: it has none
     * SDL can drive - why() says. */
    bool open(SDL_Joystick *joy);
    void close();                           /* the forces stopped, the device let go */
    bool is_open() const { return h_ != nullptr; }
    bool has_constant() const;              /* it plays a constant force (the steering force) */
    bool has_spring() const;                /* ... a spring (the centring spring) */
    const char *why() const { return why_; }
    /* The forces played as they are now (an effect updated only when its
     * values change; the spring stopped while off). */
    void apply(const Forces &f, const Settings &s);

private:
    SDL_Haptic *h_ = nullptr;
    unsigned features_ = 0;
    int constant_ = -1, spring_ = -1;       /* the effects (-1: not made yet) */
    int level_ = 0;                         /* the constant force's level sent */
    bool springOn_ = false;
    int springCoef_ = -1, springSat_ = -1;  /* the spring's sent */
    bool restartLogged_ = false;            /* (PC 2026-11-22: the workaround logged since open) */
    char why_[160] = "";
};

} // namespace wheel_ffb
