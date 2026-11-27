/* clutch_input.h - PC (2026-11-09, no original): the players' clutches and
 * H-pattern shifters as the game reads them each frame, for the realistic
 * gear shift's clutch mode (game/app/pc_clutch.h; gt3pc.ini [Game]
 * ShiftBehavior=clutch).
 *
 * User request: "Clutch button: Each controller and wheel device get one.
 * Clutch design: Analog. ...  Launcher: H-Shifter 1 and 2 are separate
 * virtual devices in the dropdown next to the controllers and wheels.  They
 * hold Neutral, 1st through 9th.  H-Shifter doesn't need double binds, but
 * any device should be able to map to it including a keyboard or mouse."
 *
 * The clutch: the Clutch control of Controller N's or Wheel N's profile
 * (input_map.h PAD_CLUTCH / WHEEL_CLUTCH) - the profile's own device (the
 * port's gamepad, the wheel's joystick), the devices its bindings name and
 * the keyboard / mouse, the larger of its slots, 0 released .. 1 pressed (a
 * button 0 / 1, an axis its travel; a wheel's after its ClutchDeadzone, PC
 * 2026-11-10).  The gate: H-Shifter N's (input_map.h
 * kHShifter1 / 2: Neutral, 1st .. 9th, Reverse) - the gate whose binding
 * is held; none held: neutral when
 * the last gate was a device's (a shifter's lever out of its gate), else
 * (a key, a mouse button) the last gate kept, so a keyboard's gates are
 * taps; a held Neutral binding neutral.  While a menu (F2, Esc) or the
 * unlocked debug camera has the input: the clutch released, the gate kept.
 * Any thread. */
#pragma once

namespace clutch_input {

/* Player `player' (0, 1)'s clutch, 0 .. 1, from Wheel N's profile (wheel)
 * or Controller N's; -1 when that profile maps no clutch. */
float clutch(int player, bool wheel);

/* Player `player''s gate: 0 neutral, 1 .. 9, 10 the reverse (HS_REVERSE);
 * -1 when H-Shifter N maps nothing. */
int gate(int player);

/* PC (2026-11-23): one of H-Shifter N's bindings is held now (a gate, the
 * neutral, the reverse) - whatever the gear shift behavior (the game's
 * notice that the H-shifter is off, game/app/pc_clutch.h); false when it
 * maps nothing or a menu has the input.  Leaves gate()'s state alone. */
bool gate_held(int player);

/* What profile `profile''s Clutch is mapped to, for the notices ("" when
 * none): its slots' labels (input_map::describe), " / " between them. */
void clutch_text(int profile, char *out, int n);

} // namespace clutch_input
