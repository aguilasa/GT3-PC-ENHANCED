/* window_test: the SDL3 game window (src/platform/sdl/window_sdl.cpp) with
 * its keyboard and mouse, against what the Windows one gives the game.
 *
 *   window_test map <gt3pc.ini>
 *       No display needed (SDL's dummy driver will do): SDL key events made
 *       up and pumped through the window's loop, their virtual keys checked
 *       (letters and digits by keycode, the rest by scancode, the keypad by
 *       Num Lock, the modifiers' generic and side keys, the hotkeys, the
 *       mouse's buttons, the keys' names); and the Windows build's gt3pc.ini
 *       (window_test_win.ini: Controller 1's Cross=button:south;key:A) read
 *       here, the key A's press making that binding 1.
 *   window_test keys <gt3pc.ini> <seconds> [--free-f2] [--expect 0xNN,..]
 *                    [--absent 0xNN,..]
 *       A real window (X11 / Wayland) at 100,100, 640x480, for <seconds>:
 *       prints `ready xid=<X11 window>' and then each virtual key pressed
 *       (take_key_press), the side and generic modifier keys held, the
 *       hotkeys run, and Controller 1's Cross binding's value going to 1;
 *       keys come from outside (xdotool).  --free-f2: the settings menu's
 *       hotkey none (written to the ini first), so F2 reaches the game as a
 *       key.  Exits 1 when an --expect key was not pressed or an --absent
 *       one was.
 *   window_test modes [WxH]
 *       A real window: windowed, borderless fullscreen, exclusive
 *       fullscreen (the display mode closest to WxH, default 800x600) and
 *       windowed again, each applied from another thread as the GPU module
 *       does, with the window's and the display's measured sizes printed.
 *
 * The window's partners are stand-ins here: the settings (the display mode
 * and the window's size), the menus and panels (their hotkeys printed), the
 * debug camera (no mouse look) and the log (standard output). */
#include "platform/cockpit_osd.h"
#include "platform/debug_camera.h"
#include "platform/frame_monitor.h"
#include "platform/frame_step.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/os/win_compat.h"
#include "platform/screenshot.h"
#include "platform/settings.h"
#include "platform/settings_menu.h"
#include "platform/song_osd.h"
#include "platform/system_menu.h"
#include "platform/texture_menu.h"
#include "platform/window.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

/* ---- stand-ins of the window's partners ---------------------------------- */

namespace {
settings::Display g_display;
std::atomic<int> g_settingsToggles{0};
} // namespace

namespace settings {
const Display &display()
{
    return g_display;
}
void save_window(int x, int y, int w, int h)
{
    printf("settings: window %d,%d %dx%d\n", x, y, w, h);
}
const char *mode_name(DisplayMode m)
{
    return m == DisplayMode::windowed ? "windowed" : m == DisplayMode::borderless ? "borderless" : "fullscreen";
}
} // namespace settings

namespace pclog {
void write(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}
} // namespace pclog

#define HOTKEY_STUB(ns, fn)                                                          \
    namespace ns {                                                                   \
    void fn() { printf("hotkey: " #ns "::" #fn "\n"); fflush(stdout); }               \
    }
HOTKEY_STUB(texture_menu, toggle)
HOTKEY_STUB(texture_menu, replace_key)
HOTKEY_STUB(texture_menu, refresh_key)
HOTKEY_STUB(texture_menu, hide)
HOTKEY_STUB(system_menu, toggle)
HOTKEY_STUB(system_menu, hide)
HOTKEY_STUB(settings_menu, hide)
HOTKEY_STUB(screenshot, request)
HOTKEY_STUB(frame_step, key_step)
HOTKEY_STUB(frame_step, key_resume)
HOTKEY_STUB(frame_monitor, toggle)
HOTKEY_STUB(debug_camera, key)
HOTKEY_STUB(song_osd, toggle)
HOTKEY_STUB(cockpit_osd, save_key)
HOTKEY_STUB(cockpit_osd, cancel_key)
HOTKEY_STUB(cockpit_osd, reset_key)
namespace settings_menu {
void toggle()
{
    g_settingsToggles++;
    printf("hotkey: settings_menu::toggle\n");
    fflush(stdout);
}
bool shown() { return false; }      /* (the menu's keys: the game's here) */
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

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond);        \
            g_failures++;                                                           \
        }                                                                           \
    } while (0)

#define CHECK_STR(got, want)                                                        \
    do {                                                                            \
        const std::string g_ = (got), w_ = (want);                                  \
        if (g_ != w_) {                                                             \
            fprintf(stderr, "%s:%d: FAIL: %s\n  got:  [%s]\n  want: [%s]\n",       \
                    __FILE__, __LINE__, #got, g_.c_str(), w_.c_str());              \
            g_failures++;                                                           \
        }                                                                           \
    } while (0)

const char *vk_label(int vk)
{
    static char buf[16];
    switch (vk) {
    case VK_RETURN: return "VK_RETURN";
    case VK_SHIFT: return "VK_SHIFT";
    case VK_CONTROL: return "VK_CONTROL";
    case VK_MENU: return "VK_MENU";
    case VK_LSHIFT: return "VK_LSHIFT";
    case VK_RSHIFT: return "VK_RSHIFT";
    case VK_LCONTROL: return "VK_LCONTROL";
    case VK_RCONTROL: return "VK_RCONTROL";
    case VK_LMENU: return "VK_LMENU";
    case VK_RMENU: return "VK_RMENU";
    case VK_ESCAPE: return "VK_ESCAPE";
    case VK_SPACE: return "VK_SPACE";
    case VK_LBUTTON: return "VK_LBUTTON";
    case VK_END: return "VK_END";
    case VK_NUMPAD1: return "VK_NUMPAD1";
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, sizeof buf, "VK_F%d", vk - VK_F1 + 1);
        return buf;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(buf, sizeof buf, "'%c'", vk);
        return buf;
    }
    snprintf(buf, sizeof buf, "0x%02X", vk);
    return buf;
}

std::set<int> take_presses()
{
    std::set<int> s;
    for (int vk = 1; vk < 256; vk++)
        if (window::take_key_press(vk))
            s.insert(vk);
    return s;
}

std::string set_text(const std::set<int> &s)
{
    std::string t;
    for (int vk : s) {
        if (!t.empty())
            t += ' ';
        t += vk_label(vk);
    }
    return t;
}

/* Controller 1's Cross, keyboard slot: key:A in window_test_win.ini */
const input_map::Binding &cross_key()
{
    return input_map::profile(input_map::kController1).map[input_map::PAD_CROSS][1];
}

float binding_value(const input_map::Binding &b)
{
    const input_map::Sources s = {nullptr, nullptr, window::key_down, 0, nullptr, 0};
    return input_map::value(b, s);
}

void push_key(SDL_Scancode sc, SDL_Keycode kc, SDL_Keymod mod, bool down)
{
    SDL_Event e = {};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID((SDL_Window *)window::handle());
    e.key.scancode = sc;
    e.key.key = kc;
    e.key.mod = mod;
    e.key.down = down;
    SDL_PushEvent(&e);
}

void tap(SDL_Scancode sc, SDL_Keycode kc, SDL_Keymod mod = SDL_KMOD_NONE)
{
    push_key(sc, kc, mod, true);
    push_key(sc, kc, mod, false);
}

/* (checks thread) Waits until the window's loop, on the main thread, has
 * handled the events pushed so far: the queue empty, then a call run on the
 * main thread (SDL runs those between two events). */
void pump()
{
    while (SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0)
        SDL_Delay(1);
    SDL_RunOnMainThread([](void *) {}, nullptr, true);
}

std::string copy_ini(const char *from, const char *name)
{
    const char *tmp = getenv("TMPDIR");
    std::string to = std::string(tmp && *tmp ? tmp : "/tmp") + "/" + name;
    FILE *in = fopen(from, "rb");
    FILE *out = fopen(to.c_str(), "wb");
    if (in == nullptr || out == nullptr) {
        fprintf(stderr, "cannot copy %s to %s\n", from, to.c_str());
        exit(2);
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return to;
}

void free_f2(const std::string &ini)
{
    input_map::save_hotkey(ini.c_str(), input_map::kHotSettingsMenu, input_map::KeyCombo(), input_map::Binding());
}

int map_checks();
std::string g_mapIni;

int run_map(const char *ini)
{
    if (getenv("SDL_VIDEO_DRIVER") == nullptr)
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    /* the Windows build's ini, read here: the same binding */
    g_mapIni = copy_ini(ini, "window_test_map.ini");
    const std::string &copy = g_mapIni;
    input_map::init(copy.c_str());
    char text[64];
    CHECK(cross_key().source == input_map::Source::key);
    CHECK(cross_key().index == 0x41);
    input_map::format(cross_key(), text, sizeof text);
    CHECK_STR(text, "key:A");
    printf("ini: [Controller1] Cross keyboard slot %s (VK 0x%02X)\n", text, cross_key().index);
    CHECK(input_map::hotkey(input_map::kHotSettingsMenu).vk == VK_F2);

    g_display.windowW = 640;
    g_display.windowH = 480;
    CHECK(window::create("window_test"));
    if (g_failures)
        return 1;
    int result = 0;
    std::thread checks([&] {
        result = map_checks();
        SDL_Event quit = {};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    });
    window::run_message_loop();
    checks.join();
    return result;
}

/* (a thread of its own: the window's loop runs on the main thread) */
int map_checks()
{
    char text[64];
    SDL_Window *win = (SDL_Window *)window::handle();
    printf("driver %s, window %p\n", SDL_GetCurrentVideoDriver(), (void *)win);

    /* letters by keycode: the US A; the AZERTY A key (scancode Q, keycode a) */
    tap(SDL_SCANCODE_A, SDLK_A);
    tap(SDL_SCANCODE_Q, SDLK_A);
    tap(SDL_SCANCODE_RETURN, SDLK_RETURN);
    tap(SDL_SCANCODE_KP_ENTER, SDLK_KP_ENTER);
    pump();
    std::set<int> got = take_presses();
    printf("A, AZERTY A, Enter, keypad Enter: %s\n", set_text(got).c_str());
    CHECK(got == (std::set<int>{VK_RETURN, 'A'}));
    CHECK(!got.count('Q'));

    /* digits by keycode, by scancode when the layout's key is no digit
     * (AZERTY's 1: keycode &); the keypad by Num Lock */
    tap(SDL_SCANCODE_1, (SDL_Keycode)'&');
    tap(SDL_SCANCODE_KP_1, SDLK_KP_1, SDL_KMOD_NUM);
    pump();
    got = take_presses();
    printf("AZERTY 1, keypad 1 (Num Lock): %s\n", set_text(got).c_str());
    CHECK(got == (std::set<int>{'1', VK_NUMPAD1}));
    tap(SDL_SCANCODE_KP_1, SDLK_END);
    pump();
    got = take_presses();
    printf("keypad 1 (no Num Lock): %s\n", set_text(got).c_str());
    CHECK(got == (std::set<int>{VK_END}));

    /* the modifiers: the generic key and the side's, held */
    push_key(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, SDL_KMOD_LSHIFT, true);
    pump();
    got = take_presses();
    printf("left Shift down: %s; held VK_SHIFT %d VK_LSHIFT %d VK_RSHIFT %d\n", set_text(got).c_str(),
           window::key_down(VK_SHIFT), window::key_down(VK_LSHIFT), window::key_down(VK_RSHIFT));
    CHECK(got == (std::set<int>{VK_SHIFT, VK_LSHIFT}));
    CHECK(window::key_down(VK_SHIFT) && window::key_down(VK_LSHIFT) && !window::key_down(VK_RSHIFT));
    push_key(SDL_SCANCODE_RSHIFT, SDLK_RSHIFT, SDL_KMOD_SHIFT, true);
    push_key(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, SDL_KMOD_RSHIFT, false);
    pump();
    take_presses();
    CHECK(window::key_down(VK_SHIFT) && !window::key_down(VK_LSHIFT) && window::key_down(VK_RSHIFT));
    push_key(SDL_SCANCODE_RSHIFT, SDLK_RSHIFT, SDL_KMOD_NONE, false);
    pump();
    printf("both Shifts up: held VK_SHIFT %d\n", window::key_down(VK_SHIFT));
    CHECK(!window::key_down(VK_SHIFT) && !window::key_down(VK_RSHIFT));

    /* F2: the settings menu's hotkey (not the game's key), as on Windows */
    const int toggles = g_settingsToggles;
    tap(SDL_SCANCODE_F2, SDLK_F2);
    pump();
    got = take_presses();
    printf("F2 (hotkey): presses [%s], settings menu toggled %d\n", set_text(got).c_str(),
           g_settingsToggles - toggles);
    CHECK(got.empty());
    CHECK(g_settingsToggles == toggles + 1);
    /* Alt+Enter: the full-screen request (windowed -> borderless) */
    push_key(SDL_SCANCODE_RETURN, SDLK_RETURN, SDL_KMOD_LALT, true);
    pump();
    settings::DisplayMode req;
    CHECK(window::take_mode_request(&req) && req == settings::DisplayMode::borderless);
    CHECK(take_presses().empty());

    /* F2 with the hotkey none: the game's VK_F2 */
    const std::string &copy = g_mapIni;
    free_f2(copy);
    input_map::init(copy.c_str());
    CHECK(input_map::hotkey(input_map::kHotSettingsMenu).vk == 0);
    tap(SDL_SCANCODE_F2, SDLK_F2);
    pump();
    got = take_presses();
    printf("F2 (no hotkey): %s\n", set_text(got).c_str());
    CHECK(got == (std::set<int>{VK_F2}));

    /* the Windows ini's key:A binding: 1 while A is held */
    push_key(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_NONE, true);
    pump();
    const float held = binding_value(cross_key());
    push_key(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_NONE, false);
    pump();
    const float released = binding_value(cross_key());
    printf("Cross (key:A): %.0f held, %.0f released\n", held, released);
    CHECK(held == 1.0f && released == 0.0f);
    take_presses();

    /* the mouse's buttons as keys, the wheel's notches */
    SDL_Event b = {};
    b.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    b.button.windowID = SDL_GetWindowID(win);
    b.button.button = SDL_BUTTON_LEFT;
    b.button.down = true;
    SDL_PushEvent(&b);
    SDL_Event w = {};
    w.type = SDL_EVENT_MOUSE_WHEEL;
    w.wheel.windowID = SDL_GetWindowID(win);
    w.wheel.integer_y = 2;
    w.wheel.y = 2;
    SDL_PushEvent(&w);
    pump();
    got = take_presses();
    uint32_t up, down;
    window::mouse_wheel(&up, &down);
    printf("left button, wheel 2 up: %s, wheel %u up %u down\n", set_text(got).c_str(), up, down);
    CHECK(got == (std::set<int>{VK_LBUTTON}) && window::key_down(VK_LBUTTON));
    CHECK(up == 2 && down == 0);

    /* the keys' names (SDL's, the layout's) */
    const struct {
        int vk;
        const char *want;
    } kNames[] = {{'A', "A"}, {VK_RETURN, "Return"}, {VK_F2, "F2"}, {VK_LSHIFT, "Left Shift"},
                  {VK_SHIFT, "Shift"}, {VK_NUMPAD1, "Keypad 1"}, {VK_OEM_1, ";"}, {VK_PRIOR, "PageUp"}};
    for (const auto &k : kNames) {
        input_map::Binding kb;
        kb.source = input_map::Source::key;
        kb.index = (uint8_t)k.vk;
        input_map::describe(kb, text, sizeof text);
        printf("name of %s: %s\n", vk_label(k.vk), text);
        CHECK_STR(text, k.want);
    }
    CHECK(window::sdl_scancode('A') == SDL_SCANCODE_A && window::sdl_scancode(VK_F2) == SDL_SCANCODE_F2);
    CHECK(window::sdl_scancode(VK_LBUTTON) == 0);

    printf("%s\n", g_failures ? "map: FAILED" : "map: ok");
    return g_failures ? 1 : 0;
}

std::set<int> parse_vks(const char *list)
{
    std::set<int> s;
    for (const char *p = list; *p;) {
        char *end;
        const long v = strtol(p, &end, 0);
        if (end == p)
            break;
        s.insert((int)v);
        p = *end == ',' ? end + 1 : end;
    }
    return s;
}

int run_keys(const char *ini, int seconds, bool freeF2, const std::set<int> &expect, const std::set<int> &absent)
{
    const std::string copy = copy_ini(ini, "window_test_keys.ini");
    if (freeF2)
        free_f2(copy);
    input_map::init(copy.c_str());
    g_display.windowW = 640;
    g_display.windowH = 480;
    g_display.windowPlaced = true;
    g_display.windowX = 100;
    g_display.windowY = 100;
    if (!window::create("window_test keys"))
        return 2;
    SDL_Window *win = (SDL_Window *)window::handle();
    const long long xid = (long long)SDL_GetNumberProperty(SDL_GetWindowProperties(win),
                                                           SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(win, &x, &y);
    window::client_size(&w, &h);
    printf("ready xid=%lld at %d,%d client %dx%d driver %s, F2 hotkey %s\n", xid, x, y, w, h,
           SDL_GetCurrentVideoDriver(), freeF2 ? "none" : "F2");
    fflush(stdout);

    std::set<int> seen;
    std::thread watch([&] {
        const uint64_t end = SDL_GetTicks() + (uint64_t)seconds * 1000;
        bool crossOn = false;
        while (SDL_GetTicks() < end) {
            for (int vk = 1; vk < 256; vk++)
                if (window::take_key_press(vk)) {
                    seen.insert(vk);
                    printf("press %s (0x%02X)\n", vk_label(vk), vk);
                    fflush(stdout);
                }
            const bool on = binding_value(cross_key()) >= 1.0f;
            if (on != crossOn) {
                printf("binding [Controller1] Cross key:A %s\n", on ? "on" : "off");
                fflush(stdout);
                crossOn = on;
            }
            SDL_Delay(1);
        }
        SDL_Event quit = {};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    });
    window::run_message_loop();
    watch.join();
    printf("seen: %s\n", set_text(seen).c_str());
    int bad = 0;
    for (int vk : expect)
        if (!seen.count(vk)) {
            printf("missing: %s\n", vk_label(vk));
            bad++;
        }
    for (int vk : absent)
        if (seen.count(vk)) {
            printf("unexpected: %s\n", vk_label(vk));
            bad++;
        }
    printf("%s\n", bad ? "keys: FAILED" : "keys: ok");
    return bad ? 1 : 0;
}

void measure(const char *what)
{
    /* (SDL's window calls on its main thread, as the window's own) */
    struct M {
        const char *what;
    } m = {what};
    SDL_RunOnMainThread(
        [](void *p) {
            SDL_Window *win = (SDL_Window *)window::handle();
            int cw, ch, x = 0, y = 0, sw = 0, sh = 0;
            window::client_size(&cw, &ch);
            SDL_GetWindowPosition(win, &x, &y);
            SDL_GetWindowSize(win, &sw, &sh);
            const SDL_WindowFlags f = SDL_GetWindowFlags(win);
            const SDL_DisplayMode *fm = SDL_GetWindowFullscreenMode(win);
            const SDL_DisplayID d = SDL_GetDisplayForWindow(win);
            const SDL_DisplayMode *cur = SDL_GetCurrentDisplayMode(d);
            const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(d);
            char fms[48] = "desktop";
            if (fm != nullptr)
                snprintf(fms, sizeof fms, "%dx%d@%.0f", fm->w, fm->h, fm->refresh_rate);
            printf("%-22s mode %-10s client %dx%d window %d,%d %dx%d fullscreen-flag %d borderless %d "
                   "fullscreen-mode %s display %dx%d@%.0f (desktop %dx%d)\n",
                   static_cast<M *>(p)->what, settings::mode_name(window::mode()), cw, ch, x, y, sw, sh,
                   (f & SDL_WINDOW_FULLSCREEN) != 0, (f & SDL_WINDOW_BORDERLESS) != 0, fms, cur ? cur->w : 0,
                   cur ? cur->h : 0, cur ? cur->refresh_rate : 0.0f, desk ? desk->w : 0, desk ? desk->h : 0);
            fflush(stdout);
        },
        &m, true);
}

int run_modes(int fw, int fh)
{
    g_display.windowW = 640;
    g_display.windowH = 480;
    g_display.windowPlaced = true;
    g_display.windowX = 100;
    g_display.windowY = 100;
    g_display.width = fw;
    g_display.height = fh;
    if (!window::create("window_test modes"))
        return 2;
    SDL_Window *win = (SDL_Window *)window::handle();
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(win), &n);
    printf("driver %s, display modes:", SDL_GetCurrentVideoDriver());
    for (int i = 0; i < n; i++)
        printf(" %dx%d@%.0f", modes[i]->w, modes[i]->h, modes[i]->refresh_rate);
    printf("%s\n", n ? "" : " none listed");
    SDL_free(modes);
    printf("xid=%lld\n", (long long)SDL_GetNumberProperty(SDL_GetWindowProperties(win),
                                                         SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    fflush(stdout);
    const int holdMs = getenv("WINDOW_TEST_HOLD_MS") ? atoi(getenv("WINDOW_TEST_HOLD_MS")) : 300;
    std::thread gpu([&] {
        /* as the GPU module: from another thread */
        SDL_Delay(holdMs);
        measure("windowed (created)");
        const settings::DisplayMode order[] = {settings::DisplayMode::borderless, settings::DisplayMode::fullscreen,
                                               settings::DisplayMode::windowed};
        for (settings::DisplayMode m : order) {
            window::apply_mode(m);
            SDL_Delay(holdMs);
            char what[32];
            snprintf(what, sizeof what, "%s (applied)", settings::mode_name(m));
            measure(what);
            CHECK(window::mode() == m);
        }
        SDL_Event quit = {};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    });
    window::run_message_loop();
    gpu.join();
    printf("%s\n", g_failures ? "modes: FAILED" : "modes: ok");
    return g_failures ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc >= 3 && strcmp(argv[1], "map") == 0)
        return run_map(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "keys") == 0) {
        bool freeF2 = false;
        std::set<int> expect, absent;
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--free-f2") == 0)
                freeF2 = true;
            else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc)
                expect = parse_vks(argv[++i]);
            else if (strcmp(argv[i], "--absent") == 0 && i + 1 < argc)
                absent = parse_vks(argv[++i]);
        }
        return run_keys(argv[2], atoi(argv[3]), freeF2, expect, absent);
    }
    if (argc >= 2 && strcmp(argv[1], "modes") == 0) {
        int w = 800, h = 600;
        if (argc >= 3)
            sscanf(argv[2], "%dx%d", &w, &h);
        return run_modes(w, h);
    }
    fprintf(stderr, "usage: window_test map <ini> | keys <ini> <seconds> [--free-f2] [--expect 0xNN,..] "
                    "[--absent 0xNN,..] | modes [WxH]\n");
    return 2;
}
