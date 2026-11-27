/* wheel_ffb.cpp - a wheel's force feedback through SDL3's haptics
 * (wheel_ffb.h). */
#include "platform/wheel_ffb.h"
#include "platform/log.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace wheel_ffb {
namespace {

/* The constant force's level sign for a push to the right, on the steering
 * axis (SDL_HAPTIC_STEERING_AXIS: SDL picks the wheel's axis).  SDL calls a
 * direction where a force comes from; on one axis the level's sign alone
 * says which way, and which way that is was not checked on a wheel here (no
 * wheel with force feedback on the PC this was written on): the launcher's
 * testers push left and right, FfbInvert turns it round. */
constexpr int kRightSign = 1;

/* a force below this (1 / 4 of the game's least step, 1 / 127) is none */
constexpr float kNone = 0.002f;

/* PC (2026-11-22): an effect's runs when started - without end, as PCSX2's
 * (usb-pad-sdl-ff.cpp: SDL_RunHapticEffect(..., SDL_HAPTIC_INFINITY) for
 * every effect; its effects' length infinite too, as here).  Was 1 (the
 * length infinite alone). */
constexpr Uint32 kIterations = SDL_HAPTIC_INFINITY;

int clamp_level(float v)
{
    const float l = v * 32767.0f;
    return l > 32767.0f ? 32767 : l < -32767.0f ? -32767 : (int)lroundf(l);
}

} // namespace

float scaled(float force, const Settings &s)
{
    float f = force * (float)s.strength / 100.0f;
    if (fabsf(f) < kNone)
        return 0.0f;
    const float m = (float)s.minimum / 100.0f;
    const float a = fabsf(f) > 1.0f ? 1.0f : fabsf(f);
    f = (f < 0.0f ? -1.0f : 1.0f) * (m + (1.0f - m) * a);
    return s.invert != 0 ? -f : f;
}

bool Device::open(SDL_Joystick *joy)
{
    close();
    why_[0] = '\0';
    if (joy == nullptr) {
        snprintf(why_, sizeof(why_), "no joystick");
        return false;
    }
    if (!SDL_IsJoystickHaptic(joy)) {
        snprintf(why_, sizeof(why_), "SDL finds no force feedback on it");
        return false;
    }
    h_ = SDL_OpenHapticFromJoystick(joy);
    if (h_ == nullptr) {
        snprintf(why_, sizeof(why_), "SDL could not open its force feedback (%s)", SDL_GetError());
        return false;
    }
    features_ = SDL_GetHapticFeatures(h_);
    if (!(features_ & (SDL_HAPTIC_CONSTANT | SDL_HAPTIC_SPRING))) {
        snprintf(why_, sizeof(why_), "its force feedback plays neither a constant force nor a spring");
        close();
        return false;
    }
    /* the device's own centring off (the game's spring and forces instead),
     * its gain whole (the strength setting scales the forces) */
    if (features_ & SDL_HAPTIC_AUTOCENTER)
        SDL_SetHapticAutocenter(h_, 0);
    if (features_ & SDL_HAPTIC_GAIN)
        SDL_SetHapticGain(h_, 100);
    return true;
}

void Device::close()
{
    if (h_ != nullptr) {
        SDL_StopHapticEffects(h_);
        if (constant_ >= 0)
            SDL_DestroyHapticEffect(h_, constant_);
        if (spring_ >= 0)
            SDL_DestroyHapticEffect(h_, spring_);
        SDL_CloseHaptic(h_);
    }
    h_ = nullptr;
    features_ = 0;
    constant_ = spring_ = -1;
    level_ = 0;
    springOn_ = false;
    springCoef_ = springSat_ = -1;
    restartLogged_ = false;                 /* (PC 2026-11-22) */
}

bool Device::has_constant() const { return (features_ & SDL_HAPTIC_CONSTANT) != 0; }
bool Device::has_spring() const { return (features_ & SDL_HAPTIC_SPRING) != 0; }

void Device::apply(const Forces &f, const Settings &s)
{
    if (h_ == nullptr)
        return;
    /* PC (2026-11-22): the FFB loss workaround (Settings::restart) */
    const bool restart = s.restart != 0;
    if (restart && !restartLogged_) {
        pclog::write("wheel: force feedback: the loss workaround on - the steering force started again with each "
                     "one the game sends (PCSX2's FfbDropoutWorkaround)");
        restartLogged_ = true;
    }
    /* the steering force: a constant force without end, its level updated */
    if (has_constant()) {
        const int level = kRightSign * clamp_level(scaled(f.force, s));
        if (constant_ < 0 && level != 0) {
            SDL_HapticEffect e;
            memset(&e, 0, sizeof(e));
            e.type = SDL_HAPTIC_CONSTANT;
            e.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
            e.constant.length = SDL_HAPTIC_INFINITY;
            e.constant.level = (Sint16)level;
            constant_ = SDL_CreateHapticEffect(h_, &e);
            if (constant_ < 0) {
                pclog::write("wheel: force feedback: no constant force (%s)", SDL_GetError());
                features_ &= ~SDL_HAPTIC_CONSTANT;
            } else if (!SDL_RunHapticEffect(h_, constant_, kIterations)) {
                pclog::write("wheel: force feedback: the constant force did not start (%s)", SDL_GetError());
            }
            level_ = level;
        } else if (constant_ >= 0) {
            bool start = restart && f.sent;     /* (PC 2026-11-22: the workaround, each force sent) */
            if (level != level_) {
                SDL_HapticEffect e;
                memset(&e, 0, sizeof(e));
                e.type = SDL_HAPTIC_CONSTANT;
                e.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
                e.constant.length = SDL_HAPTIC_INFINITY;
                e.constant.level = (Sint16)level;
                /* (a driver that stops an effect on its update, when it says
                 * so: started again) */
                if (!SDL_UpdateHapticEffect(h_, constant_, &e) ||
                    ((features_ & SDL_HAPTIC_STATUS) && !SDL_GetHapticEffectStatus(h_, constant_)))
                    start = true;
                level_ = level;
            }
            if (start)
                SDL_RunHapticEffect(h_, constant_, kIterations);
        }
    }
    /* the centring spring: a spring about the centre, both sides alike */
    if (has_spring()) {
        const float k = f.springK * (float)s.spring / 100.0f, c = f.springClip * (float)s.spring / 100.0f;
        const bool on = f.spring && k > 0.0f && c > 0.0f;
        const int coef = (int)lroundf((k > 1.0f ? 1.0f : k) * 32767.0f);
        const int sat = (int)lroundf((c > 1.0f ? 1.0f : c) * 65535.0f);
        if (on && (coef != springCoef_ || sat != springSat_ || spring_ < 0)) {
            SDL_HapticEffect e;
            memset(&e, 0, sizeof(e));
            e.type = SDL_HAPTIC_SPRING;
            e.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
            e.condition.length = SDL_HAPTIC_INFINITY;
            e.condition.right_coeff[0] = e.condition.left_coeff[0] = (Sint16)coef;
            e.condition.right_sat[0] = e.condition.left_sat[0] = (Uint16)sat;
            if (spring_ < 0) {
                spring_ = SDL_CreateHapticEffect(h_, &e);
                if (spring_ < 0) {
                    pclog::write("wheel: force feedback: no spring (%s)", SDL_GetError());
                    features_ &= ~SDL_HAPTIC_SPRING;
                    return;
                }
            } else {
                SDL_UpdateHapticEffect(h_, spring_, &e);
            }
            springCoef_ = coef;
            springSat_ = sat;
        }
        /* (the workaround leaves the spring alone, as PCSX2's does) */
        if (on != springOn_ && spring_ >= 0) {
            if (on)
                SDL_RunHapticEffect(h_, spring_, kIterations);
            else
                SDL_StopHapticEffect(h_, spring_);
            springOn_ = on;
        }
    }
}

} // namespace wheel_ffb
