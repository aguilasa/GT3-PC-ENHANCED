/* pc_ffb.h - PC (2026-11-20): the wheels' force feedback, the game's side
 * (the platform plays the forces: platform/wheel_ffb.h).
 *
 * GT3's own Force Feedback option (its Options menu; GameOptions
 * forceFeedback1p / 2p, kept in the save) gates the race's steering force
 * (RaceInput::sendVibration): off, the wheel gets none whatever the PC's
 * strength.  User report: "the in-game setting for FFB was off" - races had
 * no force feedback and the PC's settings did not say why.  So the option is
 * told to the platform every tick (the settings menu's FORCE FEEDBACK
 * STRENGTH reads "(IN-GAME OPTION IS OFF)" then: platform/wheel.h game_ffb)
 * and a live race that begins with the player's wheel as their device and
 * the option off shows a notice (user: "at the start of a race, an overlay
 * popup should appear alerting the user if FFB is off and while it is
 * taking inputs from the wheel device (Both connected under controller mode
 * wouldn't pop this)"). */
#pragma once

/* GT3's option told to the platform (pc_frame_tick_begin, once a tick). */
void pc_ffb_sync(void);
/* A live race begins for `player' (0, 1), whose wheel is their device, with
 * GT3's option off: the notice, 8 s (RaceInput::pcFfbNotice). */
void pc_ffb_notice(int player);
