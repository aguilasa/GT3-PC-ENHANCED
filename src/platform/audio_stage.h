/* audio_stage.h - the output stage of GT3 PC's sound (audio.h), shared by
 * the output back ends: Windows' kernel streaming and WASAPI
 * (platform/audio.cpp) and SDL elsewhere (platform/sdl/audio_sdl.cpp).
 *
 * Stage renders the native SPU2 (snd_native_render) at 48 kHz and applies
 * what audio.h describes: the master gain on the game's sound, the music
 * files added after it, a DC blocker, a peak limiter, the debug dump
 * (GT3PC_AUDIO_DUMP), the tick step's pause fade and a volume the back end
 * sets (kernel streaming: the Windows volumes).  Converter takes it to the
 * output's rate (cubic interpolation).  Both run on the output's real-time
 * thread: no allocation, lock or log in render. */
#pragma once

#include <stdint.h>
#include <stdio.h>

#include <atomic>
#include <vector>

namespace audio {

constexpr int kRate = 48000;                    /* the SPU2's rate */
constexpr int kMaxFrames = 16384;               /* a render call's frames at most (a converter's source) */

struct Stage {
    float x1[2] = {0, 0}, y1[2] = {0, 0};
    float gain = 1.0f;                          /* the limiter's */
    float vol = 1.0f;                           /* the back end's volume (volume, glided) */
    float run = 1.0f;                           /* 0 while the tick step pauses (faded) */
    uint32_t limited = 0;                       /* frames turned down (logged) */
    std::atomic<float> volume{1.0f};            /* the back end's volume: kernel streaming's Windows volumes */
    FILE *dump = nullptr;
    uint32_t dumpFrames = 0;
    uint32_t dumpSinceHeader = 0;
    std::vector<int16_t> dumpBuf;

    /* GT3PC_AUDIO_DUMP=<file.wav>: the mix written to it too. */
    void open_dump();
    /* The dump's header brought up to date and the file closed. */
    void close_dump();
    void write_header();
    /* Renders `frames' 48 kHz stereo frames into out (float, after the
     * stage); now: when the first is rendered (sched::now_seconds). */
    void render(float *out, int frames, double now);
};

/* 48 kHz to the output's rate: Stage's frames, cubic interpolation. */
struct Converter {
    Stage *stage;
    int rate = kRate;
    double step = 1.0;                          /* source frames per output frame */
    double frac = 0.0;                          /* the position between h[1] and h[2] */
    float h[4][2] = {};                         /* the last four source frames */
    std::vector<float> src;

    explicit Converter(Stage *s) : stage(s) {}
    void reset(int outRate);
    /* `frames' stereo float frames at the output's rate; now: when the
     * first is rendered.  The source frames a call takes are rendered at
     * once (one engine render per output period: its blocks' timing). */
    void render(float *out, int frames, double now);
};

} // namespace audio
