/* audio.h - the sound output of GT3 PC (docs/sound.md 3.5).
 *
 * On the PS2 the SPU2 mixed 48 voices at 48 kHz and sent the result to the
 * DAC on its own.  In the port the SPU2 is the native model of
 * engine/sound/native (sound_native.h), run as a scheduled engine: each
 * state block the game sends is applied at the sample its vblank maps to,
 * and this module renders it (snd_native_render) on the output's real-time
 * thread, a few milliseconds ahead of the speaker:
 *
 *  - kernel streaming (platform/audio_ks.h): straight to the driver's render
 *    pin, no Windows audio engine; periods of [Audio] Buffer ms (default 2),
 *    the device holding about two.  A crackle (an underrun, after the
 *    stream's first 0.5 s) raises the buffer by 0.5 ms (gt3pc.ini; the
 *    output restarts); a stream that still fails at 20 ms leaves Output=auto
 *    to WASAPI until Output changes.  Each says so over the picture for 2 s
 *    ("Audio buffer insufficient. ...", "Persistent insufficient ...").  A pin of one instance (most onboard
 *    and HDMI devices) is the game's alone while it plays, as with ASIO:
 *    other programs are silent on that device - not changed, but said (the
 *    log, a notice over the picture once, the settings menu); when another
 *    program holds such a pin, kernel streaming cannot start.  The Windows
 *    audio engine applied the game's volume mixer slider, and the device's
 *    volume and mute where they are software: read every 50 ms and applied
 *    by the stage instead;
 *  - WASAPI shared (the fallback of Output=auto): through the Windows audio
 *    engine at its smallest period (IAudioClient3 where the driver allows,
 *    else the engine's default, ~10 ms);
 *  - off Windows, SDL3 (platform/sdl/audio_sdl.cpp): a stream in pull mode
 *    on the default device, through the same output stage
 *    (platform/audio_stage.h); every Output value means it;
 *  - none: no device (or Output=ks and the device busy): the engine still
 *    renders at real-time pace, so the game's voices end and the BGM clock
 *    goes on; the output is tried again every 2 s.
 *
 * It follows the default device (reopens when it changes or is lost),
 * restarts when [Audio] changes (the settings menu) and keeps playing when
 * the window is in the background, as a console would.  The output's rate
 * and channels are the device's: 48 kHz stereo is converted (cubic
 * interpolation; L / R to the first two channels).
 *
 * Output stage (48 kHz float): a master gain (-4 dB, as PCSX2 scales the
 * SPU2's output) on the game's sound; the music files (snd_native_render_
 * music: a .wav / .mp3 BGM) added after it at their own level, straight, as
 * Windows plays them; a DC blocker at ~4 Hz (the console's analog output is
 * AC-coupled) and a peak limiter at 0 dBFS (instant attack, 100 ms release:
 * a music file alone passes untouched): the engine mixes without the SPU2's
 * 16-bit clamps (headroom), so a loud pile-up is turned down, not clipped;
 * then, with kernel streaming, the Windows volumes (glided over ~4 ms).
 *
 * Debug: with the environment variable GT3PC_AUDIO_DUMP=<file.wav> every
 * mixed sample (after the output stage, 48 kHz 16-bit) is also written to
 * that WAV; GT3PC_SND_TRACE=1 / 2 traces the engine into the log. */
#pragma once

#include <stddef.h>

namespace audio {

/* Starts the native SPU2 (snd_native_init, scheduled on sched's clock) and
 * the output thread.  Call after sched::init, before the game threads
 * start. */
void init();
/* Stops the output thread (the process may also just end). */
void shutdown();
/* The output running, for the settings menu: "KS 4.2 + 3.1 MS" (the
 * output's latency + the scheduling delay), "WASAPI ...", "NONE"; off
 * Windows "SDL ..." (platform/sdl/audio_sdl.cpp). */
void describe(char *out, size_t n);

} // namespace audio
