/* audio_stage.cpp - the output stage of GT3 PC's sound (audio_stage.h):
 * gain, DC blocker, limiter, the debug dump, the rate converter. */
#include "platform/audio_stage.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "engine/sound/native/sound_native.h"
#include "platform/frame_step.h"
#include "platform/log.h"

namespace audio {
namespace {

constexpr float kMasterGain = 0.63f;            /* -4 dB: the game's sound (not the music files) */
constexpr float kDcPole = 0.9995f;              /* DC blocker pole: -3 dB at ~3.8 Hz */
constexpr float kLimit = 1.0f;                  /* the limiter's ceiling: 0 dBFS (a music file alone: untouched) */
constexpr float kLimitRelease = 2.08e-4f;       /* its gain back to 1 per sample: 100 ms (1 - e^(-1/4800)) */
constexpr float kVolumeGlide = 0.005f;          /* the volume's step to a new value per sample (~4 ms) */
constexpr float kPauseFade = 1.0f / 240.0f;     /* the tick step's fade a sample (5 ms) */

} // namespace

/* ---- the output stage: gain, DC blocker, limiter, the debug dump ------- */

void Stage::open_dump()
{
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("GT3PC_AUDIO_DUMP", path, sizeof(path));
    if (n == 0 || n >= sizeof(path))
        return;
#else
    const char *path = getenv("GT3PC_AUDIO_DUMP");
    if (path == nullptr || path[0] == '\0')
        return;
#endif
    dump = fopen(path, "wb");
    if (!dump) {
        pclog::write("audio: cannot write the dump %s", path);
        return;
    }
    dumpBuf.resize(2 * kMaxFrames);
    write_header();
    pclog::write("audio: dumping the mix to %s", path);
}

void Stage::close_dump()
{
    if (dump) {
        write_header();
        fclose(dump);
        dump = nullptr;
    }
}

void Stage::write_header()
{
    uint32_t data = dumpFrames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    uint16_t s = 1;
    memcpy(h + 20, &s, 2);              /* PCM */
    s = 2;
    memcpy(h + 22, &s, 2);              /* stereo */
    v = kRate;
    memcpy(h + 24, &v, 4);
    v = kRate * 4;
    memcpy(h + 28, &v, 4);
    s = 4;
    memcpy(h + 32, &s, 2);
    s = 16;
    memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(dump, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), dump);
    fseek(dump, 0, SEEK_END);
    fflush(dump);
}

void Stage::render(float *out, int frames, double now)
{
    /* the tick step's pause (platform/frame_step.h): faded out, then the
     * mix stops - the voices and the BGM stream hold where they are -
     * until it runs again (faded in) */
    const bool paused = frame_step::paused();
    if (paused && run <= 0.0f) {
        memset(out, 0, sizeof(float) * 2 * (size_t)frames);
        return;
    }
    snd_native_render(out, frames, now);
    /* the game's sound at the master gain, the music files after it at
     * their own level (straight) */
    for (int i = 0; i < 2 * frames; i++)
        out[i] *= kMasterGain;
    snd_native_render_music(out, frames);
    for (int i = 0; i < frames; i++) {
        float lr[2];
        for (int c = 0; c < 2; c++) {
            const float x = out[2 * i + c];
            const float y = x - x1[c] + kDcPole * y1[c];
            x1[c] = x;
            y1[c] = y;
            lr[c] = y;
        }
        /* the limiter: stereo-linked, down at once to the ceiling, back
         * up slowly */
        const float peak = fabsf(lr[0]) > fabsf(lr[1]) ? fabsf(lr[0]) : fabsf(lr[1]);
        const float want = peak > kLimit ? kLimit / peak : 1.0f;
        if (want < gain)
            gain = want;
        else
            gain += (want - gain) * kLimitRelease;
        if (gain < 0.999f)
            limited++;
        out[2 * i] = lr[0] * gain;
        out[2 * i + 1] = lr[1] * gain;
    }
    if (dump) {
        for (int i = 0; i < 2 * frames; i++)
            dumpBuf[i] = (int16_t)lrintf(out[i] * 32767.0f);
        fwrite(dumpBuf.data(), 4, frames, dump);
        dumpFrames += frames;
        dumpSinceHeader += frames;
        if (dumpSinceHeader >= (uint32_t)kRate) {
            dumpSinceHeader = 0;
            write_header();             /* a valid file at any time: the process may just end */
        }
    }
    /* the back end's volume (kernel streaming: the Windows volumes it
     * bypasses; after the dump: it keeps the game's own level) */
    const float target = volume.load(std::memory_order_relaxed);
    const float runTarget = paused ? 0.0f : 1.0f;
    if (vol != 1.0f || target != 1.0f || run != 1.0f || runTarget != 1.0f) {
        for (int i = 0; i < frames; i++) {
            vol += (target - vol) * kVolumeGlide;
            if (fabsf(target - vol) < 1e-5f)
                vol = target;
            run = runTarget > run ? std::min(1.0f, run + kPauseFade) : std::max(0.0f, run - kPauseFade);
            out[2 * i] *= vol * run;
            out[2 * i + 1] *= vol * run;
        }
    }
}

/* ---- the rate converter: 48 kHz to the output's (cubic) ---------------- */

void Converter::reset(int outRate)
{
    rate = outRate;
    step = (double)kRate / (double)outRate;
    frac = 0.0;
    memset(h, 0, sizeof(h));
    src.assign(2 * kMaxFrames, 0.0f);
}

void Converter::render(float *out, int frames, double now)
{
    if (rate == kRate) {
        while (frames > 0) {
            const int n = frames < kMaxFrames ? frames : kMaxFrames;
            stage->render(out, n, now);
            out += 2 * n;
            frames -= n;
            now += (double)n / kRate;
        }
        return;
    }
    const int maxOut = (int)((kMaxFrames - 2) / step);
    while (frames > maxOut) {               /* (never: a period is far shorter) */
        render(out, maxOut, now);
        out += 2 * maxOut;
        frames -= maxOut;
        now += (double)maxOut / rate;
    }
    int need = 0;
    double f = frac;
    for (int i = 0; i < frames; i++) {
        f += step;
        while (f >= 1.0) {
            f -= 1.0;
            need++;
        }
    }
    if (need > 0)
        stage->render(src.data(), need, now);
    int k = 0;
    for (int i = 0; i < frames; i++) {
        const float t = (float)frac;
        for (int c = 0; c < 2; c++) {
            /* Catmull-Rom between h[1] and h[2] */
            const float p0 = h[0][c], p1 = h[1][c], p2 = h[2][c], p3 = h[3][c];
            const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
            const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
            const float cc = -0.5f * p0 + 0.5f * p2;
            out[2 * i + c] = ((a * t + b) * t + cc) * t + p1;
        }
        frac += step;
        while (frac >= 1.0) {
            frac -= 1.0;
            memmove(h[0], h[1], sizeof(h[0]) * 3);
            h[3][0] = src[2 * k];
            h[3][1] = src[2 * k + 1];
            k++;
        }
    }
}

} // namespace audio
