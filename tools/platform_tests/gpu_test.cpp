/* gpu_test: the Direct3D 11 device off Windows (src/platform/gpu.cpp on
 * DXVK-native) on the SDL3 game window (src/platform/sdl/window_sdl.cpp).
 * Both need a display and a Vulkan device: without a display the program
 * exits 77 (ctest's skip).
 *
 *   gpu_test frames [N] [--vsync N] [--hold-ms MS]
 *       The device and the swap chain on a 640x480 window; N frames (120 by
 *       default), each cleared with one colour and presented.  The colour
 *       read back from the back buffer of the last frame, no present failed
 *       (the GPU module's log), and the frame rate over the frames after the
 *       first ten printed, with the adapter, its video memory and DXGI's
 *       counts (also as the log's memory line, platform/memstat.h).  --vsync: the vertical sync setting (0 off, the default; N:
 *       Present(1) with N frames queued).  --hold-ms: the colour presented
 *       for that long after (a screenshot; `ready xid=' printed first).
 *   gpu_test modes [WxH]
 *       Windowed, then exclusive fullscreen in the display mode closest to
 *       WxH (default 800x600) and windowed again, each a mode request taken
 *       by the next frame's back buffer as the game's Alt+Enter: the
 *       display's mode, the back buffer and the swap chain's exclusive state
 *       printed after each.  Exits 1 unless the fullscreen mode was the
 *       display's while in it and the desktop's mode came back after.
 *
 * Run with LD_LIBRARY_PATH at DXVK-native's and SDL3's libraries.  The GPU
 * module sets DXVK_WSI_DRIVER=SDL3 when the environment has none; DXVK's own
 * variables choose the device (DXVK_FILTER_DEVICE_NAME) and show it
 * (DXVK_HUD=devinfo).  The module's partners are stand-ins here: the
 * settings, the scheduler (run_outside calls straight), the frame monitor,
 * the menus and panels, and the log (standard output). */
#include "platform/frame_monitor.h"
#include "platform/gpu.h"
#include "platform/log.h"
#include "platform/memstat.h"
#include "platform/scheduler.h"
#include "platform/settings.h"
#include "platform/window.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <math.h>
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

/* ---- stand-ins of the module's partners ----------------------------------- */

namespace {
settings::Display g_display;
int g_vsync;
std::mutex g_logMutex;
std::vector<std::string> g_log;
} // namespace

namespace settings {
const Display &display()
{
    return g_display;
}
int vsync()
{
    return g_vsync;
}
void save_window(int, int, int, int) {}
const char *mode_name(DisplayMode m)
{
    return m == DisplayMode::windowed ? "windowed" : m == DisplayMode::borderless ? "borderless" : "fullscreen";
}
} // namespace settings

namespace pclog {
void write(const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    printf("log: %s\n", text);
    fflush(stdout);
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_log.push_back(text);
}
void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(3);
}
} // namespace pclog

namespace sched {
void run_outside(void (*fn)(void *), void *arg)
{
    fn(arg);
}
double now_seconds()
{
    return (double)SDL_GetTicksNS() * 1e-9;
}
} // namespace sched

namespace frame_monitor {
void add(Part, double) {}
void present_begin() {}
void presented() {}
void toggle() {}
} // namespace frame_monitor

#define HOTKEY_STUB(ns, fn)                                                          \
    namespace ns {                                                                   \
    void fn() {}                                                                     \
    }
HOTKEY_STUB(texture_menu, toggle)
HOTKEY_STUB(texture_menu, replace_key)
HOTKEY_STUB(texture_menu, refresh_key)
HOTKEY_STUB(texture_menu, hide)
HOTKEY_STUB(system_menu, toggle)
HOTKEY_STUB(system_menu, hide)
HOTKEY_STUB(settings_menu, hide)
HOTKEY_STUB(settings_menu, toggle)
HOTKEY_STUB(screenshot, request)
HOTKEY_STUB(frame_step, key_step)
HOTKEY_STUB(frame_step, key_resume)
HOTKEY_STUB(debug_camera, key)
HOTKEY_STUB(song_osd, toggle)
HOTKEY_STUB(cockpit_osd, save_key)
HOTKEY_STUB(cockpit_osd, cancel_key)
HOTKEY_STUB(cockpit_osd, reset_key)
namespace settings_menu {
bool shown() { return false; }
void key(int, bool) {}
} // namespace settings_menu
namespace system_menu {
bool shown() { return false; }
void key(int, bool) {}
} // namespace system_menu
namespace texture_menu {
bool shown() { return false; }
bool key(int, bool) { return false; }
} // namespace texture_menu
namespace song_osd {
void step(int) {}
} // namespace song_osd
namespace cockpit_osd {
bool active() { return false; }
void axis_key(int) {}
void adjust_key(int) {}
} // namespace cockpit_osd
namespace debug_camera {
bool wants_mouse() { return false; }
} // namespace debug_camera

/* ---- the checks ------------------------------------------------------------ */

namespace {

int g_failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            g_failures++;                                                            \
        }                                                                            \
    } while (0)

/* the clear colour, and its B8G8R8A8 bytes */
const float kColour[4] = {0.2f, 0.6f, 0.9f, 1.0f};
const uint8_t kBgra[4] = {230, 153, 51, 255};

int log_count(const char *part)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    int n = 0;
    for (const std::string &l : g_log)
        if (l.find(part) != std::string::npos)
            n++;
    return n;
}

/* One frame: the back buffer cleared with the colour and presented. */
void frame()
{
    ID3D11RenderTargetView *rtv = gpu::backbuffer();
    if (rtv != nullptr)
        gpu::context()->ClearRenderTargetView(rtv, kColour);
    gpu::present();
}

/* The pixel at the middle of the back buffer, read back (false: not read). */
bool read_middle(uint8_t bgra[4])
{
    ID3D11RenderTargetView *rtv = gpu::backbuffer();
    if (rtv == nullptr)
        return false;
    gpu::context()->ClearRenderTargetView(rtv, kColour);
    ID3D11Resource *res = nullptr;
    rtv->GetResource(&res);
    ID3D11Texture2D *bb = nullptr;
    const bool isTex = res != nullptr && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&bb));
    if (res != nullptr)
        res->Release();
    if (!isTex)
        return false;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D *st = nullptr;
    bool ok = false;
    if (SUCCEEDED(gpu::device()->CreateTexture2D(&sd, nullptr, &st))) {
        gpu::context()->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(gpu::context()->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const uint8_t *p = (const uint8_t *)m.pData + (size_t)(d.Height / 2) * m.RowPitch + (d.Width / 2) * 4;
            memcpy(bgra, p, 4);
            gpu::context()->Unmap(st, 0);
            ok = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        st->Release();
    }
    bb->Release();
    gpu::present();
    return ok;
}

long long window_xid()
{
    return (long long)SDL_GetNumberProperty(SDL_GetWindowProperties((SDL_Window *)window::handle()),
                                            SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
}

void print_memory()
{
    uint64_t local = 0, budget = 0, shared = 0;
    if (gpu::video_memory(&local, &budget, &shared))
        printf("video memory (DXGI): local %u MB of %u, non-local %u MB\n", (unsigned)(local >> 20),
               (unsigned)(budget >> 20), (unsigned)(shared >> 20));
    else
        printf("video memory (DXGI): not known\n");
    char line[256];
    memstat::line(line, sizeof line, true);
    printf("memory line: %s\n", line);
}

/* (game thread) frames */
void frame_checks(int n, int holdMs)
{
    const bool made = gpu::init(window::handle());
    CHECK(made);
    if (!made)
        return;
    print_memory();
    const int warm = n > 20 ? 10 : 0;
    double t0 = sched::now_seconds();
    for (int i = 0; i < n - 1; i++) {
        if (i == warm)
            t0 = sched::now_seconds();
        frame();
    }
    uint8_t px[4] = {};
    const bool read = read_middle(px);           /* (the last frame) */
    const double t1 = sched::now_seconds();
    const double fps = (n - warm) / (t1 - t0);
    int interval = 0;
    bool tearing = false, exclusive = false;
    gpu::present_info(&interval, &tearing, &exclusive);
    int w = 0, h = 0;
    gpu::backbuffer_size(&w, &h);
    printf("back buffer %dx%d, last frame's middle pixel BGRA %u %u %u %u (expected %u %u %u %u)\n", w, h, px[0], px[1],
           px[2], px[3], kBgra[0], kBgra[1], kBgra[2], kBgra[3]);
    CHECK(read);
    for (int k = 0; k < 4; k++)
        CHECK(abs((int)px[k] - (int)kBgra[k]) <= 1);
    const int failed = log_count("failed");
    printf("frames: %d presented, %d failed, %.1f fps over the last %d (vsync %d: interval %d, tearing %s), "
           "refresh %.2f Hz\n",
           n, failed, fps, n - warm, g_vsync, interval, tearing ? "yes" : "no", gpu::refresh_hz());
    CHECK(failed == 0);
    print_memory();
    if (holdMs > 0) {
        printf("ready xid=%lld\n", window_xid());
        fflush(stdout);
        const double end = sched::now_seconds() + holdMs / 1000.0;
        while (sched::now_seconds() < end) {
            frame();
            SDL_Delay(16);
        }
    }
}

struct Seen {
    int dispW, dispH;
    float dispHz;
    int bbW, bbH;
    bool exclusive;
    settings::DisplayMode mode;
};

Seen look(const char *what)
{
    SDL_Window *win = (SDL_Window *)window::handle();
    const SDL_DisplayMode *cur = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
    Seen s = {};
    s.dispW = cur ? cur->w : 0;
    s.dispH = cur ? cur->h : 0;
    s.dispHz = cur ? cur->refresh_rate : 0.0f;
    gpu::backbuffer_size(&s.bbW, &s.bbH);
    int interval = 0;
    bool tearing = false;
    gpu::present_info(&interval, &tearing, &s.exclusive);
    s.mode = window::mode();
    printf("%s: mode %s, display %dx%d@%.2f, back buffer %dx%d, exclusive %s, refresh %.2f Hz\n", what,
           settings::mode_name(s.mode), s.dispW, s.dispH, s.dispHz, s.bbW, s.bbH, s.exclusive ? "yes" : "no",
           gpu::refresh_hz());
    fflush(stdout);
    return s;
}

/* (game thread) the modes */
void mode_checks(int holdMs)
{
    const bool made = gpu::init(window::handle());
    CHECK(made);
    if (!made)
        return;
    for (int i = 0; i < 30; i++)
        frame();
    const Seen desk = look("windowed (created)");
    CHECK(desk.bbW == 640 && desk.bbH == 480 && !desk.exclusive);

    window::request_mode(settings::DisplayMode::fullscreen);
    for (int i = 0; i < 30; i++)
        frame();
    SDL_Delay(holdMs);
    frame();
    const Seen full = look("fullscreen (requested)");
    /* the mode SDL picks for the size asked */
    SDL_DisplayMode want = {};
    const bool known = SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow((SDL_Window *)window::handle()),
                                                           g_display.width, g_display.height, 60.0f, false, &want);
    printf("closest display mode to %dx%d: %s %dx%d@%.2f\n", g_display.width, g_display.height, known ? "" : "(none)",
           want.w, want.h, want.refresh_rate);
    CHECK(full.mode == settings::DisplayMode::fullscreen);
    CHECK(full.exclusive);
    CHECK(known && full.dispW == want.w && full.dispH == want.h);
    CHECK(full.bbW == full.dispW && full.bbH == full.dispH);

    window::request_mode(settings::DisplayMode::windowed);
    for (int i = 0; i < 30; i++)
        frame();
    SDL_Delay(holdMs);
    frame();
    const Seen back = look("windowed (restored)");
    CHECK(back.mode == settings::DisplayMode::windowed && !back.exclusive);
    CHECK(back.dispW == desk.dispW && back.dispH == desk.dispH && back.dispHz == desk.dispHz);
    CHECK(back.bbW == 640 && back.bbH == 480);
    CHECK(log_count("failed") == 0);
}

void quit()
{
    SDL_Event q = {};
    q.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&q);
}

} // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const bool frames = argc >= 2 && strcmp(argv[1], "frames") == 0;
    const bool modes = argc >= 2 && strcmp(argv[1], "modes") == 0;
    if (!frames && !modes) {
        fprintf(stderr, "usage: gpu_test frames [N] [--vsync N] [--hold-ms MS] | modes [WxH] [--hold-ms MS]\n");
        return 2;
    }
    if (getenv("DISPLAY") == nullptr && getenv("WAYLAND_DISPLAY") == nullptr) {
        printf("no display: skipped\n");
        return 77;
    }
    int n = 120, holdMs = 0;
    g_display.width = 800;
    g_display.height = 600;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--vsync") == 0 && i + 1 < argc)
            g_vsync = atoi(argv[++i]);
        else if (strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc)
            holdMs = atoi(argv[++i]);
        else if (frames)
            n = atoi(argv[i]);
        else
            sscanf(argv[i], "%dx%d", &g_display.width, &g_display.height);
    }
    if (modes && holdMs == 0)
        holdMs = 500;
    g_display.mode = settings::DisplayMode::windowed;
    g_display.windowW = 640;
    g_display.windowH = 480;
    g_display.windowPlaced = true;
    g_display.windowX = 100;
    g_display.windowY = 100;
    if (!window::create("gpu_test")) {
        fprintf(stderr, "no window\n");
        return 1;
    }
    printf("SDL %s, DXVK_WSI_DRIVER %s\n", SDL_GetCurrentVideoDriver(),
           getenv("DXVK_WSI_DRIVER") ? getenv("DXVK_WSI_DRIVER") : "(unset)");
    std::thread game([&] {
        if (frames)
            frame_checks(n, holdMs);
        else
            mode_checks(holdMs);
        quit();
    });
    window::run_message_loop();
    game.join();
    printf("%s: %s\n", frames ? "frames" : "modes", g_failures ? "FAILED" : "ok");
    return g_failures ? 1 : 0;
}
