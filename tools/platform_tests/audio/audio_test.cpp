/* audio_test - the sound output off Windows (src/platform/sdl/audio_sdl.cpp,
 * the shared stage src/platform/audio_stage.cpp) with the native SPU2 and
 * the BGM player of engine/pdi/T521_pdibgm.cpp, recorded by SDL's disk
 * driver instead of a sound device:
 *
 *   SDL_AUDIO_DRIVER=disk audio_test ads FILE.ads SECONDS OUT.raw
 *       plays SECONDS of the .ads (its BGM clock); passes when the
 *       recording's RMS > 0
 *   SDL_AUDIO_DRIVER=disk audio_test mp3 FILE.mp3 EXPECTED_SECONDS OUT.raw
 *       plays the .mp3 to its end; passes when its decoded length is
 *       EXPECTED_SECONDS within 1 % and the recording's RMS > 0
 *   SDL_AUDIO_DRIVER=disk audio_test silence - SECONDS OUT.raw
 *       the control: no BGM; passes when the recording is all zeros
 *
 * The recording is in the device's format (the disk driver's: the stream's,
 * F32 stereo at SDL's default 44.1 kHz; F32 or S16 read).  GT3PC_TEST_BUFFER_MS
 * sets [Audio] Buffer (default 2).  The disk driver paces itself by whole
 * milliseconds, so short periods run faster than real time; the BGM clock,
 * not the wall clock, measures the music.  It refuses to run without
 * SDL_AUDIO_DRIVER=disk: it never plays on a real device. */
#include <SDL3/SDL.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "platform/audio.h"
#include "platform/disc.h"
#include "platform/frame_step.h"
#include "platform/host_file.h"
#include "platform/log.h"
#include "platform/scheduler.h"
#include "platform/settings.h"
#include "platform/ui_text.h"

/* bgm_side.cpp (the decomp's options) */
void bgm_side_start(int sectors, int loop, const char *hostName);
void bgm_side_stop(void);
float bgm_side_status(int *ended);

/* ---- what the output and T521 need of the platform ------------------------ */

using Clock = std::chrono::steady_clock;
static const Clock::time_point g_t0 = Clock::now();

namespace sched {
double now_seconds()
{
    return std::chrono::duration<double>(Clock::now() - g_t0).count();
}
} // namespace sched

namespace settings {
/* [Audio] Buffer: 2 ms (the default), or GT3PC_TEST_BUFFER_MS */
double audio_buffer_ms()
{
    const char *e = getenv("GT3PC_TEST_BUFFER_MS");
    return e != nullptr && atof(e) > 0.0 ? atof(e) : 2.0;
}
int audio_generation() { return 0; }
int volume(Volume) { return 100; }
} // namespace settings

namespace frame_step {
bool paused() { return false; }
} // namespace frame_step

namespace ui_text {
const char *tr(const char *english) { return english; }
} // namespace ui_text

namespace pclog {
void vwrite(const char *fmt, va_list ap)
{
    printf("log: ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
}
void write(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwrite(fmt, ap);
    va_end(ap);
}
} // namespace pclog

namespace disc {
bool read_bytes(uint64_t, void *, uint32_t) { return false; }
} // namespace disc

extern "C" {
int host_file_open(const char *path) { return open(path, O_RDONLY | O_CLOEXEC); }
void host_file_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
int64_t host_file_size(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? (int64_t)st.st_size : -1;
}
int host_file_read_at(int fd, uint64_t offset, void *buf, uint32_t bytes)
{
    const ssize_t n = pread(fd, buf, bytes, (off_t)offset);
    return n < 0 ? -1 : (int)n;
}
}

/* ---- the test ----------------------------------------------------------- */

static void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void print_describe(const char *when)
{
    char d[64];
    audio::describe(d, sizeof(d));
    printf("describe() %s: %s\n", when, d);
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "audio_test ads|mp3|silence FILE SECONDS OUT.raw\n");
        return 2;
    }
    const char *drv = getenv("SDL_AUDIO_DRIVER");
    if (drv == nullptr || strcmp(drv, "disk") != 0) {
        fprintf(stderr, "audio_test: SDL_AUDIO_DRIVER=disk is required (no real device)\n");
        return 2;
    }
    const std::string mode = argv[1];
    const char *file = argv[2];
    const double seconds = atof(argv[3]);
    const char *out = argv[4];
    if ((mode != "ads" && mode != "mp3" && mode != "silence") || seconds <= 0.0) {
        fprintf(stderr, "audio_test: bad arguments\n");
        return 2;
    }
    SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, out);

    audio::init();
    sleep_ms(300);                              /* (the output thread opens the device) */
    print_describe("after init");
    SDL_AudioSpec spec = {};
    int frames = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames)) {
        fprintf(stderr, "audio_test: no device format (%s)\n", SDL_GetError());
        return 1;
    }
    printf("device: driver %s, format 0x%04X, %d channels, %d Hz, %d frames a period\n", SDL_GetCurrentAudioDriver(),
           (unsigned)spec.format, spec.channels, spec.freq, frames);

    int rc = 0;
    double decoded = -1.0;
    if (mode == "silence") {
        sleep_ms((int)(seconds * 1000.0));
    } else {
        struct stat st;
        if (::stat(file, &st) != 0) {
            fprintf(stderr, "audio_test: %s not found\n", file);
            return 1;
        }
        const int sectors = (int)((st.st_size + disc::kSectorSize - 1) / disc::kSectorSize);
        bgm_side_start(sectors, 0, file);
        const double t0 = sched::now_seconds();
        const double limit = seconds * 1.5 + 3.0;  /* (wall clock: the most it may take) */
        double nextPrint = 1.0;
        while (sched::now_seconds() - t0 < limit) {
            sleep_ms(20);
            int ended = 0;
            const float at = bgm_side_status(&ended);
            const double t = sched::now_seconds() - t0;
            if (t >= nextPrint) {
                printf("  %.1f s: BGM at %.3f s%s\n", t, at, ended ? " (ended)" : "");
                nextPrint += 1.0;
            }
            if (mode == "ads" && at >= seconds) {
                decoded = at;
                printf("  %.1f s: BGM at %.3f s: stopped\n", t, at);
                break;
            }
            if (mode == "mp3" && ended) {
                decoded = at;
                printf("  %.1f s: BGM ended at %.6f s\n", t, at);
                break;
            }
        }
        print_describe("while playing");
        bgm_side_stop();
    }
    audio::shutdown();                          /* (SDL's audio quits: the recording closed) */

    /* the recording */
    FILE *f = fopen(out, "rb");
    if (f == nullptr) {
        fprintf(stderr, "audio_test: no recording %s\n", out);
        return 1;
    }
    std::vector<uint8_t> raw;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        raw.insert(raw.end(), buf, buf + n);
    fclose(f);
    const bool isFloat = spec.format == SDL_AUDIO_F32;
    if (!isFloat && spec.format != SDL_AUDIO_S16) {
        fprintf(stderr, "audio_test: the device format 0x%04X is not F32 or S16\n", (unsigned)spec.format);
        return 1;
    }
    const size_t bytes = isFloat ? 4 : 2;
    const size_t samples = raw.size() / bytes;
    double sum = 0.0, peak = 0.0;
    for (size_t i = 0; i < samples; i++) {
        double v;
        if (isFloat) {
            float x;
            memcpy(&x, &raw[i * 4], 4);
            v = x;
        } else {
            int16_t x;
            memcpy(&x, &raw[i * 2], 2);
            v = x / 32768.0;
        }
        sum += v * v;
        peak = fabs(v) > peak ? fabs(v) : peak;
    }
    const double rms = samples > 0 ? sqrt(sum / (double)samples) : 0.0;
    const double recorded = spec.channels > 0 && spec.freq > 0 ? (double)samples / spec.channels / spec.freq : 0.0;
    printf("recording %s: %zu bytes, %.2f s, RMS %.6f (%.1f dBFS), peak %.6f\n", out, raw.size(), recorded, rms,
           rms > 0.0 ? 20.0 * log10(rms) : -INFINITY, peak);

    if (mode == "silence") {
        if (rms != 0.0) {
            printf("FAIL: the control's recording is not silent\n");
            rc = 1;
        } else {
            printf("PASS: no BGM, RMS 0\n");
        }
    } else {
        if (!(rms > 0.0)) {
            printf("FAIL: RMS 0\n");
            rc = 1;
        }
        if (mode == "ads" && (decoded < seconds || recorded < seconds)) {
            printf("FAIL: under %.1f s played (BGM %.3f s, %.2f s recorded)\n", seconds, decoded, recorded);
            rc = 1;
        }
        if (mode == "mp3") {
            const double err = decoded < 0.0 ? 1.0 : fabs(decoded - seconds) / seconds;
            printf("decoded length %.6f s, expected %.6f s: %.3f %% off\n", decoded, seconds, err * 100.0);
            if (err > 0.01) {
                printf("FAIL: the decoded length is more than 1 %% off\n");
                rc = 1;
            }
        }
        if (rc == 0)
            printf("PASS\n");
    }
    return rc;
}
