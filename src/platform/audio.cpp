/* audio.cpp - the sound output of GT3 PC (audio.h) on Windows: kernel
 * streaming, WASAPI as the fallback (the output stage: audio_stage.h). */
#include "platform/audio.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <avrt.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "engine/sound/native/sound_native.h"
#include "platform/audio_ks.h"
#include "platform/audio_stage.h"
#include "platform/frame_step.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/scheduler.h"
#include "platform/settings.h"
#include "platform/settings_menu.h"
#include "platform/ui_text.h"                  /* PC (2026-11-16): the interface's languages */

namespace audio {
namespace {

constexpr int kNoDeviceStepMs = 5;              /* without a device: the render's pace */
constexpr ULONGLONG kRetryMs = 2000;            /* without a device: the next try */
constexpr double kCrackleStepMs = 0.5;          /* a crackle raises the buffer by this */
constexpr double kCrackleGrace = 0.5;           /* s after a start whose underruns do not count (the start's) */
constexpr ULONGLONG kVolumePollMs = 50;         /* kernel streaming: the Windows volumes read this often */

enum Mode { MODE_NONE, MODE_KS, MODE_WASAPI };

std::atomic<bool> g_quit{false};
std::atomic<bool> g_deviceChanged{false};
std::thread g_thread;

/* the output running (describe) */
std::atomic<int> g_mode{MODE_NONE};
std::atomic<double> g_latencyMs{0.0};
std::atomic<bool> g_ksBusy{false};              /* Output=ks and kernel streaming could not start */
std::atomic<bool> g_ksExclusive{false};         /* kernel streaming on a pin of 1 instance (others silent) */

/* sched::now_seconds minus QPC seconds (the kernel streaming's times) */
double g_qpcToSched;

double qpc_seconds()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
}

double engine_clock()
{
    return sched::now_seconds();
}

/* the output stage and the rate converter (platform/audio_stage.h) */
Stage g_stage;
Converter g_conv(&g_stage);

/* ---- kernel streaming ---------------------------------------------------- */

/* (start fills it before its first render call) */
audio_ks::Info g_ksInfo;

void ks_render(void *, float *out, int frames, double now)
{
    if (g_conv.rate != g_ksInfo.rate && g_ksInfo.rate > 0)
        g_conv.reset(g_ksInfo.rate);            /* the stream's first call */
    g_conv.render(out, frames, now + g_qpcToSched);
}

void ks_log(const char *line)
{
    pclog::write("audio: %s", line);
}

/* ---- WASAPI (shared; the fallback) --------------------------------------- */

template <class T> void release(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct Wasapi {
    IMMDevice *dev = nullptr;
    IAudioClient *client = nullptr;
    IAudioRenderClient *render = nullptr;
    HANDLE event = nullptr;
    UINT32 bufferFrames = 0, periodFrames = 0;
    int rate = kRate, channels = 2, bits = 32;
    bool isFloat = true, lowLatency = false;
    double latencyMs = 0.0;
    std::thread thread;
    std::atomic<bool> quit{false}, failed{false};
    std::vector<float> mix;

    bool ok() const { return render != nullptr; }

    void close()
    {
        quit = true;
        if (thread.joinable())
            thread.join();
        if (client)
            client->Stop();
        release(render);
        release(client);
        release(dev);
        if (event) {
            CloseHandle(event);
            event = nullptr;
        }
        failed = false;
    }

    /* the mix format's rate, channels and sample type; false: one this
     * cannot write */
    bool take_format(const WAVEFORMATEX *wf)
    {
        channels = wf->nChannels;
        rate = (int)wf->nSamplesPerSec;
        bits = wf->wBitsPerSample;
        isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            isFloat = ((const WAVEFORMATEXTENSIBLE *)wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (isFloat)
            return bits == 32;
        return bits == 16 || bits == 24 || bits == 32;
    }

    bool open(IMMDeviceEnumerator *en, double periodMs)
    {
        HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr))
            return false;
        WAVEFORMATEX *wf = nullptr;
        /* IAudioClient3: the engine's smallest period the driver allows (at
         * least periodMs) */
        IAudioClient3 *c3 = nullptr;
        lowLatency = false;
        hr = dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void **)&c3);
        if (SUCCEEDED(hr) && SUCCEEDED(c3->GetMixFormat(&wf)) && take_format(wf)) {
            UINT32 def = 0, fund = 0, minP = 0, maxP = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(wf, &def, &fund, &minP, &maxP)) && fund > 0) {
                UINT32 want = (UINT32)ceil(periodMs * rate / 1000.0);
                want = (want + fund - 1) / fund * fund;
                if (want < minP)
                    want = minP;
                if (want > maxP)
                    want = maxP;
                if (SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, want, wf, nullptr))) {
                    periodFrames = want;
                    lowLatency = true;
                    client = c3;
                    c3 = nullptr;
                }
            }
        }
        release(c3);
        if (!lowLatency) {
            /* the engine's default period */
            if (wf) {
                CoTaskMemFree(wf);
                wf = nullptr;
            }
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
            if (SUCCEEDED(hr))
                hr = client->GetMixFormat(&wf);
            if (SUCCEEDED(hr) && !take_format(wf))
                hr = E_FAIL;
            if (SUCCEEDED(hr))
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wf, nullptr);
            REFERENCE_TIME defP = 0, minP = 0;
            if (SUCCEEDED(hr) && SUCCEEDED(client->GetDevicePeriod(&defP, &minP)))
                periodFrames = (UINT32)(defP * rate / 10000000);
            if (FAILED(hr)) {
                pclog::write("audio: WASAPI: the output device refused the stream (0x%08X)", (unsigned)hr);
                if (wf)
                    CoTaskMemFree(wf);
                close();
                return false;
            }
        }
        if (wf)
            CoTaskMemFree(wf);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event || FAILED(client->SetEventHandle(event)) || FAILED(client->GetBufferSize(&bufferFrames)) ||
            FAILED(client->GetService(__uuidof(IAudioRenderClient), (void **)&render))) {
            pclog::write("audio: WASAPI: the output device could not be set up");
            close();
            return false;
        }
        REFERENCE_TIME lat = 0;
        client->GetStreamLatency(&lat);
        latencyMs = bufferFrames * 1000.0 / rate + lat / 10000.0;
        mix.assign(2 * (size_t)(bufferFrames + 16), 0.0f);
        BYTE *buf = nullptr;
        if (SUCCEEDED(render->GetBuffer(bufferFrames, &buf)))
            render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(client->Start())) {
            close();
            return false;
        }
        quit = false;
        failed = false;
        g_conv.reset(rate);                     /* (the thread's from here) */
        thread = std::thread([this] { thread_main(); });
        pclog::write("audio: WASAPI shared%s, %d Hz %d-bit%s %d channels, period %u frames, buffer %u frames "
                     "(%.1f ms latency)", lowLatency ? " (low latency)" : "", rate, bits, isFloat ? " float" : "",
                     channels, periodFrames, bufferFrames, latencyMs);
        return true;
    }

    /* The float stereo mix at the device's rate into its format. */
    void write(BYTE *buf, UINT32 frames)
    {
        for (UINT32 i = 0; i < frames; i++) {
            const float l = mix[2 * i], r = mix[2 * i + 1];
            for (int c = 0; c < channels; c++) {
                float v = channels == 1 ? (l + r) * 0.5f : (c == 0 ? l : (c == 1 ? r : 0.0f));
                v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
                const size_t at = (size_t)i * channels + c;
                if (isFloat) {
                    ((float *)buf)[at] = v;
                } else if (bits == 16) {
                    ((int16_t *)buf)[at] = (int16_t)lrintf(v * 32767.0f);
                } else if (bits == 24) {
                    const int32_t s = (int32_t)lrintf(v * 8388607.0f);
                    buf[3 * at] = (BYTE)s;
                    buf[3 * at + 1] = (BYTE)(s >> 8);
                    buf[3 * at + 2] = (BYTE)(s >> 16);
                } else {
                    ((int32_t *)buf)[at] = (int32_t)lrint(v * 2147483647.0);
                }
            }
        }
    }

    void thread_main()
    {
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD task = 0;
        HANDLE avrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (avrt)
            AvSetMmThreadPriority(avrt, AVRT_PRIORITY_CRITICAL);
        else
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        while (!quit) {
            if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0)
                continue;
            UINT32 padding = 0;
            HRESULT hr = client->GetCurrentPadding(&padding);
            if (SUCCEEDED(hr) && padding < bufferFrames) {
                const UINT32 frames = bufferFrames - padding;
                BYTE *buf = nullptr;
                hr = render->GetBuffer(frames, &buf);
                if (SUCCEEDED(hr)) {
                    g_conv.render(mix.data(), (int)frames, sched::now_seconds());
                    write(buf, frames);
                    hr = render->ReleaseBuffer(frames, 0);
                }
            }
            if (FAILED(hr)) {
                failed = true;
                break;
            }
        }
        if (avrt)
            AvRevertMmThreadCharacteristics(avrt);
        if (SUCCEEDED(hrCom))
            CoUninitialize();
    }
};

/* ---- default device changes --------------------------------------------- */

class DeviceNotify : public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = InterlockedDecrement(&refs_);
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == eRender && role == eConsole)
            g_deviceChanged = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG refs_ = 1;
};

/* ---- the Windows volumes, kernel streaming ------------------------------- */

/* Kernel streaming bypasses the Windows audio engine, which applies the
 * game's volume mixer slider (its session's volume and mute) and, on a
 * device whose volume is not in hardware, the device's volume and mute too:
 * read here (the output thread, every kVolumePollMs) and applied by the
 * stage.  A hardware volume the driver applies to every stream, ours too. */
struct Volumes {
    IAudioEndpointVolume *endpoint = nullptr;   /* the device's (null: both in hardware) */
    bool swVolume = false, swMute = false;
    ISimpleAudioVolume *session = nullptr;      /* the game's slider (its session: opened here, listed by the mixer) */

    void open(IMMDeviceEnumerator *en)
    {
        close();
        IMMDevice *dev = nullptr;
        if (en == nullptr || FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
            return;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void **)&endpoint))) {
            DWORD hw = 0;
            if (FAILED(endpoint->QueryHardwareSupport(&hw)))
                hw = 0;
            swVolume = (hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) == 0;
            swMute = (hw & ENDPOINT_HARDWARE_SUPPORT_MUTE) == 0;
            if (!swVolume && !swMute)
                release(endpoint);
        }
        IAudioSessionManager *sm = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, (void **)&sm))) {
            if (FAILED(sm->GetSimpleAudioVolume(nullptr, FALSE, &session)))
                session = nullptr;
            sm->Release();
        }
        dev->Release();
        pclog::write("audio: kernel streaming applies the game's volume mixer slider%s; the device's volume %s, its "
                     "mute %s", session ? "" : " (NOT FOUND)", swVolume ? "here (software)" : "by the driver (hardware)",
                     swMute ? "here (software)" : "by the driver (hardware)");
    }

    void close()
    {
        release(endpoint);
        release(session);
        swVolume = swMute = false;
    }

    /* the gain they ask for (1: none open) */
    float gain()
    {
        float g = 1.0f;
        BOOL mute = FALSE;
        if (endpoint) {
            float db = 0.0f;
            if (swVolume && SUCCEEDED(endpoint->GetMasterVolumeLevel(&db)))
                g *= powf(10.0f, db / 20.0f);
            if (swMute && SUCCEEDED(endpoint->GetMute(&mute)) && mute)
                g = 0.0f;
        }
        if (session) {
            float v = 1.0f;
            if (SUCCEEDED(session->GetMasterVolume(&v)))
                g *= v;
            if (SUCCEEDED(session->GetMute(&mute)) && mute)
                g = 0.0f;
        }
        return g;
    }
};

/* ---- the output thread: opens, watches and reopens the output ---------- */

Wasapi g_wasapi;
Volumes g_volumes;
bool g_ksGaveUp;                                /* Output=auto: kernel streaming failed at the largest buffer */

/* The notice over the picture when kernel streaming finds the device busy
 * (audio_ks::start_busy): Output=auto plays through WASAPI instead,
 * Output=ks has no sound and tries again every 2 s (kRetryMs). */
void notice_ks_busy(bool fallback)
{
    /* (PC 2026-11-16: in the interface's language, platform/ui_text.h - each
     * case one whole text) */
    char menu[32], text[1024];
    input_map::hotkey_text(input_map::kHotSettingsMenu, menu, sizeof(menu));
    if (fallback)
        snprintf(text, sizeof(text), "%s",
                 tr("AUDIO: KERNEL STREAMING: THE DEVICE IS BUSY\n"
                    "(WINDOWS OR ANOTHER PROGRAM IS PLAYING ON IT).\n"
                    "PLAYING THROUGH WASAPI INSTEAD."));
    else if (menu[0] != '\0')
        snprintf(text, sizeof(text),
                 tr("AUDIO: KERNEL STREAMING: THE DEVICE IS BUSY\n"
                    "(WINDOWS OR ANOTHER PROGRAM IS PLAYING ON IT).\n"
                    "NO SOUND: TRYING AGAIN EVERY 2 S.\n"
                    "OR: %s, AUDIO OUTPUT, AUTO OR WASAPI."),
                 menu);
    else
        snprintf(text, sizeof(text), "%s",
                 tr("AUDIO: KERNEL STREAMING: THE DEVICE IS BUSY\n"
                    "(WINDOWS OR ANOTHER PROGRAM IS PLAYING ON IT).\n"
                    "NO SOUND: TRYING AGAIN EVERY 2 S.\n"
                    "OR: THE SETTINGS MENU, AUDIO OUTPUT, AUTO OR WASAPI."));
    settings_menu::notice(text, 8.0);
}

void set_running(Mode mode, double latencyMs)
{
    g_mode = mode;
    g_latencyMs = latencyMs;
    snd_native_set_output_latency((int)(latencyMs * kRate / 1000.0));
}

/* Opens the output [Audio] asks for: kernel streaming, WASAPI. */
Mode open_output(IMMDeviceEnumerator *en)
{
    const settings::AudioOutput want = settings::audio_output();
    const double periodMs = settings::audio_buffer_ms();
    g_ksBusy = false;
    g_ksExclusive = false;
    if (want == settings::AudioOutput::ks ||
        (want == settings::AudioOutput::automatic && !g_ksGaveUp)) {
        audio_ks::Info &info = g_ksInfo;
        memset(&info, 0, sizeof(info));
        g_qpcToSched = sched::now_seconds() - qpc_seconds();
        g_conv.reset(kRate);
        if (audio_ks::start(ks_render, nullptr, periodMs, &info)) {
            set_running(MODE_KS, info.latencyMs);
            pclog::write("audio: kernel streaming to %s: %d Hz %d-bit%s %d channels, %s, period %d frames, "
                         "buffer %d frames (%.1f ms latency)", info.device, info.rate, info.bits,
                         info.isFloat ? " float" : "", info.channels, info.waveRt ? "WaveRT" : "standard streaming",
                         info.periodFrames, info.bufferFrames, info.latencyMs);
            g_volumes.open(en);
            g_stage.volume = g_volumes.gain();
            if (info.instances == 1) {
                /* not changed (the user's call, 2026-10-20): only said */
                static bool warned;
                g_ksExclusive = true;
                pclog::write("audio: WARNING: kernel streaming has %s to itself: other programs (voice chat, a "
                             "browser) are silent on it while the game plays; [Audio] Output=wasapi shares it",
                             info.device);
                if (!warned)
                    settings_menu::notice(tr("AUDIO: KERNEL STREAMING HAS THIS DEVICE TO ITSELF.\n"
                                             "OTHER PROGRAMS (VOICE CHAT, BROWSER) ARE SILENT ON IT\n"
                                             "WHILE THE GAME PLAYS. TO SHARE IT: F2, AUDIO OUTPUT, WASAPI."),
                                          12.0);
                warned = true;
            }
            return MODE_KS;
        }
        if (want == settings::AudioOutput::ks) {
            g_ksBusy = true;
            return MODE_NONE;
        }
        pclog::write("audio: kernel streaming not possible: WASAPI instead");
        if (audio_ks::start_busy())
            notice_ks_busy(true);
    }
    if (en) {
        if (g_wasapi.open(en, periodMs)) {
            set_running(MODE_WASAPI, g_wasapi.latencyMs);
            return MODE_WASAPI;
        }
    }
    return MODE_NONE;
}

bool timing_log();

void close_output(Mode mode)
{
    if (mode == MODE_KS) {
        audio_ks::stop();
        char line[400];
        if (timing_log() && audio_ks::timing_line(line, sizeof(line)))
            pclog::write("audio: %s; the longest wait for the sound lock %.2f ms (the last, to the stop)", line,
                         snd_native_take_render_lock_wait());
    } else if (mode == MODE_WASAPI)
        g_wasapi.close();
    g_volumes.close();
    g_stage.volume = 1.0f;
    g_ksExclusive = false;
    set_running(MODE_NONE, 0.0);
}

/* A crackle (an underrun) with kernel streaming: the buffer 0.5 ms longer
 * (written to gt3pc.ini: the output restarts with it).  At the largest:
 * false. */
bool raise_buffer(uint32_t underruns)
{
    const double cur = settings::audio_buffer_ms();
    if (cur >= settings::kAudioBufferMax - 1e-9)
        return false;
    const double next = cur + kCrackleStepMs < settings::kAudioBufferMax ? cur + kCrackleStepMs
                                                                         : settings::kAudioBufferMax;
    pclog::write("audio: a crackle (%u underrun%s) at a %.1f ms buffer: raising it to %.1f ms", underruns,
                 underruns == 1 ? "" : "s", cur, next);
    settings_menu::notice(tr("Audio buffer insufficient. Raising buffer by 0.5ms..."), 2.0);
    settings::set_audio_buffer_ms(next);
    return true;
}

void log_schedule()
{
    SndNativeScheduleInfo si;
    snd_native_schedule_info(&si);
    pclog::write("audio: scheduling delay %.1f ms, %u blocks (%u late, %u applied early), limiter on %u frames",
                 si.delayMs, si.blocks, si.lateBlocks, si.droppedBlocks, g_stage.limited);
}

/* GT3PC_AUDIO_TIMING=1 (a diagnostic): the kernel stream's refill timing in
 * the log every 10 s. */
bool timing_log()
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT3PC_AUDIO_TIMING");
        on = e != nullptr && atoi(e) > 0 ? 1 : 0;
        if (on)
            pclog::write("audio: logging the kernel stream's refill timing every 10 s (GT3PC_AUDIO_TIMING)");
    }
    return on != 0;
}

void thread_main()
{
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void **)&en);
    DeviceNotify *notify = nullptr;
    if (SUCCEEDED(hr)) {
        notify = new DeviceNotify();
        en->RegisterEndpointNotificationCallback(notify);
    } else {
        pclog::write("audio: no MMDevice enumerator (0x%08X): no WASAPI", (unsigned)hr);
    }
    audio_ks::set_log(ks_log);

    Mode mode = MODE_NONE;
    int gen = settings::audio_generation();
    settings::AudioOutput output = settings::audio_output();
    ULONGLONG nextTry = 0, nextLog = GetTickCount64() + 60000, nextVolume = 0;
    bool reported = false;
    double owed = 0.0, last = sched::now_seconds();
    uint32_t ksUnderruns = 0;                   /* the kernel stream's so far */
    double openedAt = 0.0;

    while (!g_quit) {
        const ULONGLONG tick = GetTickCount64();
        if (settings::audio_output() != output) {
            output = settings::audio_output();
            g_ksGaveUp = false;                 /* the player chose again */
        }
        /* kernel streaming: a crackle raises the buffer (the generation
         * changes: restarted below); a stream that fails at the largest
         * buffer leaves Output=auto to WASAPI */
        bool reopen = false;
        if (mode == MODE_KS) {
            const uint32_t u = audio_ks::underruns();
            const bool failed = audio_ks::failed();
            if (u != ksUnderruns) {
                const uint32_t added = u - ksUnderruns;
                ksUnderruns = u;
                const bool counted = failed || sched::now_seconds() - openedAt >= kCrackleGrace;
                if (counted && !raise_buffer(added) && failed && output == settings::AudioOutput::automatic) {
                    g_ksGaveUp = true;
                    pclog::write("audio: kernel streaming fails even at the largest buffer: WASAPI from now on "
                                 "(until [Audio] Output changes)");
                    settings_menu::notice(tr("Persistent insufficient audio buffer. Switching output..."), 2.0);
                }
            }
            if (failed) {
                pclog::write("audio: kernel streaming failed: reopening");
                reopen = true;
            }
            if (tick >= nextVolume) {
                g_stage.volume = g_volumes.gain();
                nextVolume = tick + kVolumePollMs;
            }
        }
        /* what ends the output running: a change of [Audio] or of the
         * default device, a failure */
        if (settings::audio_generation() != gen) {
            gen = settings::audio_generation();
            pclog::write("audio: the settings changed: restarting the output");
            reopen = true;
        }
        if (g_deviceChanged.exchange(false)) {
            pclog::write("audio: the default output device changed: reopening");
            reopen = true;
        }
        if (mode == MODE_WASAPI && g_wasapi.failed) {
            pclog::write("audio: WASAPI: the output device was lost: reopening");
            reopen = true;
        }
        if (reopen) {
            if (mode != MODE_NONE)
                log_schedule();
            close_output(mode);
            mode = MODE_NONE;
            nextTry = tick + 200;
            reported = false;
        }
        if (mode == MODE_NONE && tick >= nextTry) {
            mode = open_output(en);
            ksUnderruns = 0;
            openedAt = sched::now_seconds();
            nextVolume = tick + kVolumePollMs;
            if (mode == MODE_NONE) {
                if (!reported) {
                    pclog::write(g_ksBusy ? "audio: kernel streaming not possible (Output=ks): rendering on without"
                                            " a device, trying again every 2 s"
                                          : "audio: no output device: rendering on without one");
                    if (g_ksBusy && audio_ks::start_busy())
                        notice_ks_busy(false);
                }
                reported = true;
                nextTry = tick + kRetryMs;
            }
        }
        if (tick >= nextLog) {
            log_schedule();
            nextLog = tick + 60000;
        }
        /* GT3PC_AUDIO_TIMING=1: the kernel stream's refill timing every 10 s
         * (audio_ks::timing_line) */
        if (mode == MODE_KS && timing_log()) {
            char line[400];
            if (audio_ks::timing_line(line, sizeof(line)))
                pclog::write("audio: %s; the longest wait for the sound lock %.2f ms", line,
                             snd_native_take_render_lock_wait());
        }
        if (mode != MODE_NONE) {
            Sleep(20);
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
        Sleep(kNoDeviceStepMs);
    }

    close_output(mode);
    if (en) {
        if (notify)
            en->UnregisterEndpointNotificationCallback(notify);
        en->Release();
    }
    if (notify)
        notify->Release();
    if (SUCCEEDED(hrCom))
        CoUninitialize();
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
    char level[8];
    DWORD n = GetEnvironmentVariableA("GT3PC_SND_TRACE", level, sizeof(level));
    if (n > 0 && n < sizeof(level) && atoi(level) > 0) {
        snd_native_set_trace(atoi(level), trace_to_log);
        pclog::write("snd: tracing sound memory uploads, key-ons and voice reads (GT3PC_SND_TRACE=%d)",
                     atoi(level));
    }
    g_stage.open_dump();
    g_conv.reset(kRate);
    g_quit = false;
    g_thread = std::thread(thread_main);
}

void shutdown()
{
    g_quit = true;
    if (g_thread.joinable())
        g_thread.join();
    g_stage.close_dump();
}

void describe(char *out, size_t n)
{
    SndNativeScheduleInfo si;
    snd_native_schedule_info(&si);
    switch (g_mode.load()) {
    case MODE_KS:                               /* (PC 2026-11-16: exclusive or not, a string each) */
        snprintf(out, n, g_ksExclusive ? tr("KS EXCLUSIVE %.1f + %.1f MS") : tr("KS %.1f + %.1f MS"),
                 g_latencyMs.load(), si.delayMs);
        break;
    case MODE_WASAPI:
        snprintf(out, n, tr("WASAPI %.1f + %.1f MS"), g_latencyMs.load(), si.delayMs);
        break;
    default:
        snprintf(out, n, "%s", g_ksBusy ? tr("NONE (KS BUSY)") : tr("NONE"));
        break;
    }
}

} // namespace audio
