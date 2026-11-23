/* audio_sdl.cpp - the sound output of GT3 PC (audio.h) off Windows: SDL3.
 *
 * An SDL audio stream on the default playback device in pull mode: SDL's
 * audio thread asks for each device period and the stream's callback
 * renders it there, through the same output stage as Windows
 * (platform/audio_stage.h: the native SPU2, gain, DC blocker, limiter,
 * the converter to the device's rate).  The stream is fed float stereo at
 * the device's own rate, so SDL only converts the sample format and the
 * channels.  The period asked for is [Audio] Buffer ms (the hint
 * SDL_AUDIO_DEVICE_SAMPLE_FRAMES; the back end may take another, logged);
 * [Audio] Output (auto, ks, wasapi) all mean this output here.  The default
 * device is followed by SDL itself (the stream moves to a new default).
 *
 * Without a device (none at the start, SDL's audio not available) the engine
 * still renders at real-time pace, so the game's voices end and the BGM
 * clock goes on; the output is tried again every 2 s.  A change of [Audio]
 * (the settings menu) restarts it. */
#include "platform/audio.h"

#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <thread>
#include <vector>

#include "engine/sound/native/sound_native.h"
#include "platform/audio_stage.h"
#include "platform/log.h"
#include "platform/scheduler.h"
#include "platform/settings.h"
#include "platform/ui_text.h"

namespace audio {
namespace {

constexpr int kNoDeviceStepMs = 5;              /* without a device: the render's pace */
constexpr uint64_t kRetryMs = 2000;             /* without a device: the next try */
constexpr int kChunkFrames = 4096;              /* the callback renders in chunks of this at most */

std::atomic<bool> g_quit{false};
std::thread g_thread;
bool g_sdlAudio = false;                        /* SDL's audio subsystem started (init) */

/* the output running (describe) */
std::atomic<bool> g_open{false};
std::atomic<double> g_latencyMs{0.0};

Stage g_stage;
Converter g_conv(&g_stage);

SDL_AudioStream *g_stream = nullptr;
std::vector<float> g_mix;                       /* the callback's (2 * kChunkFrames) */

double engine_clock()
{
    return sched::now_seconds();
}

/* SDL's audio thread: `additional' bytes of float stereo at the device's
 * rate wanted now. */
void SDLCALL pull(void *, SDL_AudioStream *stream, int additional, int)
{
    int frames = (additional + 7) / 8;
    double now = sched::now_seconds();
    while (frames > 0) {
        const int n = frames < kChunkFrames ? frames : kChunkFrames;
        g_conv.render(g_mix.data(), n, now);
        SDL_PutAudioStreamData(stream, g_mix.data(), n * 8);
        frames -= n;
        now += (double)n / g_conv.rate;
    }
}

void set_running(bool open, double latencyMs)
{
    g_open = open;
    g_latencyMs = latencyMs;
    snd_native_set_output_latency((int)(latencyMs * kRate / 1000.0));
}

/* Opens the stream on the default playback device: false when none. */
bool open_output()
{
    if (!g_sdlAudio)
        return false;
    /* the period asked for: [Audio] Buffer (frames at the device's rate) */
    SDL_AudioSpec dev = {};
    int devFrames = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, &devFrames) || dev.freq <= 0)
        dev.freq = kRate;
    const int want = (int)(settings::audio_buffer_ms() * dev.freq / 1000.0 + 0.5);
    char hint[16];
    snprintf(hint, sizeof(hint), "%d", want > 16 ? want : 16);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, hint);
    const SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, dev.freq};
    g_conv.reset(dev.freq);                     /* (before the first callback) */
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull, nullptr);
    if (g_stream == nullptr) {
        pclog::write("audio: SDL: no output device (%s)", SDL_GetError());
        return false;
    }
    const SDL_AudioDeviceID id = SDL_GetAudioStreamDevice(g_stream);
    SDL_AudioSpec got = {};
    int frames = 0;
    if (!SDL_GetAudioDeviceFormat(id, &got, &frames) || got.freq <= 0) {
        got = spec;
        frames = want;
    }
    /* the latency: the period SDL fills plus the one the back end plays */
    const double latencyMs = 2.0 * frames * 1000.0 / got.freq;
    set_running(true, latencyMs);
    const char *name = SDL_GetAudioDeviceName(id);
    pclog::write("audio: SDL (%s) to %s: %d Hz %d channels, format 0x%04X, period %d frames (asked %d), the stream "
                 "%d Hz float stereo (%.1f ms latency)", SDL_GetCurrentAudioDriver(),
                 name != nullptr ? name : "the default device", got.freq, got.channels, (unsigned)got.format, frames,
                 want, dev.freq, latencyMs);
    if (!SDL_ResumeAudioStreamDevice(g_stream))
        pclog::write("audio: SDL: the device does not start (%s)", SDL_GetError());
    return true;
}

void close_output()
{
    if (g_stream != nullptr) {
        SDL_DestroyAudioStream(g_stream);       /* (no callback after it returns) */
        g_stream = nullptr;
    }
    set_running(false, 0.0);
}

void log_schedule()
{
    SndNativeScheduleInfo si;
    snd_native_schedule_info(&si);
    pclog::write("audio: scheduling delay %.1f ms, %u blocks (%u late, %u applied early), limiter on %u frames",
                 si.delayMs, si.blocks, si.lateBlocks, si.droppedBlocks, g_stage.limited);
}

void thread_main()
{
    int gen = settings::audio_generation();
    uint64_t nextTry = 0, nextLog = SDL_GetTicks() + 60000;
    bool open = false, reported = false;
    double owed = 0.0, last = sched::now_seconds();

    while (!g_quit) {
        const uint64_t tick = SDL_GetTicks();
        if (settings::audio_generation() != gen) {
            gen = settings::audio_generation();
            pclog::write("audio: the settings changed: restarting the output");
            if (open)
                log_schedule();
            close_output();
            open = false;
            nextTry = tick + 200;
            reported = false;
        }
        if (!open && tick >= nextTry) {
            open = open_output();
            if (!open) {
                if (!reported)
                    pclog::write("audio: no output device: rendering on without one");
                reported = true;
                nextTry = tick + kRetryMs;
            }
        }
        if (tick >= nextLog) {
            log_schedule();
            nextLog = tick + 60000;
        }
        if (open) {
            SDL_Delay(20);
            last = sched::now_seconds();
            owed = 0.0;
            continue;
        }
        /* no device: the engine renders at real-time pace */
        const double now = sched::now_seconds();
        owed += (now - last) * kRate;
        last = now;
        if (owed > kRate / 4)
            owed = kRate / 4;
        float tmp[2 * 256];
        while (owed >= 256.0) {
            g_stage.render(tmp, 256, now);
            owed -= 256.0;
        }
        SDL_Delay(kNoDeviceStepMs);
    }

    if (open)
        log_schedule();
    close_output();
}

} // namespace

/* GT3PC_SND_TRACE=1 / 2: the sound trace (sound_native.h, docs/sound.md
 * 3.6) into the log. */
static void trace_to_log(const char *line)
{
    pclog::write("%s", line);
}

void init()
{
    snd_native_init();
    /* the scheduled engine (sound_native.h): each state block applied at the
     * sample its vblank maps to, rendered by the output's thread */
    snd_native_set_scheduled(1, engine_clock);
    const char *level = getenv("GT3PC_SND_TRACE");
    if (level != nullptr && atoi(level) > 0) {
        snd_native_set_trace(atoi(level), trace_to_log);
        pclog::write("snd: tracing sound memory uploads, key-ons and voice reads (GT3PC_SND_TRACE=%d)",
                     atoi(level));
    }
    g_sdlAudio = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!g_sdlAudio)
        pclog::write("audio: SDL's audio does not start (%s)", SDL_GetError());
    g_stage.open_dump();
    g_conv.reset(kRate);
    g_mix.assign(2 * (size_t)kChunkFrames, 0.0f);
    g_quit = false;
    g_thread = std::thread(thread_main);
}

void shutdown()
{
    g_quit = true;
    if (g_thread.joinable())
        g_thread.join();
    if (g_sdlAudio) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_sdlAudio = false;
    }
    g_stage.close_dump();
}

void describe(char *out, size_t n)
{
    SndNativeScheduleInfo si;
    snd_native_schedule_info(&si);
    if (g_open.load())
        snprintf(out, n, "SDL %.1f + %.1f MS", g_latencyMs.load(), si.delayMs);
    else
        snprintf(out, n, "%s", tr("NONE"));
}

} // namespace audio
