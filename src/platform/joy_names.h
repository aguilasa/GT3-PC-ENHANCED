/* joy_names.h - PC (2026-11-23): the names joysticks go by in gt3pc.ini
 * ([WheelN] Device, a binding's `@<name>'), shared by the game (platform/
 * gamepad.cpp) and the launcher (launcher/controls.cpp) so both mean the same
 * joystick by a name.  Call from the thread that runs SDL.
 *
 * A user's report (a Fanatec ClubSport DD+): the wheel base shows as two
 * game controllers of one name - "FANATEC Wheel", the same USB ids, two HID
 * collections: one with the steering, the pedals and the force feedback (8
 * axes, 108 buttons), one without (12 axes, 63 buttons).  SDL's name alone
 * meant "the first of that name", which was the part without force feedback:
 * the game made it wheel 1, the launcher took the other part's inputs for
 * another device and wrote every binding with `@FANATEC Wheel' (which then
 * kept both parts from being a wheel at all), and its Device list could save
 * only that one name for either.
 *
 * So: a joystick's name here is SDL's, and for the 2nd, 3rd... connected
 * joystick of the same SDL name "<name> #2", "<name> #3"...  Their order:
 * the ones whose force feedback opens first, then by device path (the same
 * from run to run), then by SDL's id.  The plain name - what an older
 * gt3pc.ini holds - is therefore the part with force feedback.
 *
 * lesser(): a joystick is the lesser part of a device when another connected
 * joystick of its SDL name and USB ids opens its force feedback and it does
 * not: never an automatic wheel (it can still be named: its buttons bound
 * as `@<name> #2', or chosen as a Device). */
#pragma once

#include <SDL3/SDL.h>

#include <string>

namespace joy_names {

/* id's name ("" when SDL has none for it). */
std::string of(SDL_JoystickID id);
/* id is the lesser part of a device (above). */
bool lesser(SDL_JoystickID id);
/* SDL finds force feedback on id (SDL_IsJoystickHaptic: nothing of it is
 * opened, so a wheel keeps its own centring).  A joystick a wheel's binding
 * names (`@<name>') is a device of that wheel - its pedals, a shifter - and
 * no automatic wheel, unless it has this: then it is the wheel itself, named
 * by its own bindings (the launcher wrote such gt3pc.ini files for the
 * user's wheel base, and capturing some controls again left the rest so). */
bool haptic(SDL_JoystickID id);
/* id was removed: what was found out about it dropped (ids are not reused
 * within a run, so this only keeps the table small). */
void forget(SDL_JoystickID id);

} // namespace joy_names
