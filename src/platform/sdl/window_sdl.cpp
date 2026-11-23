/* window_sdl.cpp - the game window of GT3 PC off Windows (window.h), on SDL3.
 *
 * The same window as window.cpp's, made and pumped by the program's first
 * thread (SDL's main thread: SDL_WaitEventTimeout there), with the same
 * behaviour: the keys as the Windows virtual keys (what gt3pc.ini stores and
 * input_map, the hotkeys and the menus take), the mouse's buttons as keys,
 * its wheel's notches and its movement, the focus, the display modes and
 * the screen windows.  The calls of other threads that touch the window
 * (apply_mode, the screen windows, press_hotkey) run on the main thread
 * (SDL_RunOnMainThread), the ones that block on Windows blocking here too.
 * The controllers' plug / unplug events of the queue go to gamepad.cpp
 * (gamepad::sdl_event), which opens and closes them on this thread.
 *
 * The keys: letters and digits by the key's keycode (the layout's: the key
 * that types A is 'A', as Windows' virtual keys are), the rest by its
 * scancode (its place on a US keyboard).  Besides the generic VK_SHIFT /
 * VK_CONTROL / VK_MENU of Windows' messages, the side's (VK_LSHIFT ..
 * VK_RMENU) are down while their key is. */
#include "platform/window.h"
#include "platform/cockpit_osd.h"
#include "platform/debug_camera.h"
#include "platform/frame_monitor.h"
#include "platform/frame_step.h"
#include "platform/gamepad.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/os/win_compat.h"
#include "platform/png_image.h"
#include "platform/screenshot.h"
#include "platform/settings_menu.h"
#include "platform/song_osd.h"
#include "platform/system_menu.h"
#include "platform/texture_menu.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <stdint.h>
#include <string.h>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef GT3PC_WINDOW_ICON
/* The program's icon (gt3pc.ico, the Windows resource's) built in. */
extern "C" __attribute__((visibility("hidden"))) const unsigned char gt3pc_window_icon[], gt3pc_window_icon_end[];
__asm__(".pushsection .rodata\n"
        ".balign 4\n"
        "gt3pc_window_icon:\n"
        ".incbin \"" GT3PC_WINDOW_ICON "\"\n"
        "gt3pc_window_icon_end:\n"
        ".popsection\n");
#endif

namespace window {
namespace {

struct Rect {
    int x, y, w, h;
};

SDL_Window *g_win = nullptr;
std::atomic<int> g_clientW{640}, g_clientH{480};
std::atomic<bool> g_closing{false};
std::atomic<bool> g_focus{true};
std::atomic<bool> g_minimized{false};
std::atomic<uint8_t> g_keys[256];
std::atomic<uint8_t> g_keyPressed[256];     /* went down since the last take_key_press */
std::atomic<uint8_t> g_hotPressed[input_map::kHotkeyCount];     /* the same of the debug hotkeys (take_hotkey) */
std::atomic<uint32_t> g_wheelUp{0}, g_wheelDown{0};     /* the mouse wheel's notches (mouse_wheel) */
std::atomic<uint32_t> g_motionX{0}, g_motionY{0};       /* the mouse's raw movement, summed (mouse_motion) */
float g_motionRestX, g_motionRestY;                     /* (its fractions, kept for the next) */
bool g_relative;                                        /* relative mouse mode on (update_mouse) */
bool g_cursorHidden;
std::atomic<int> g_mode{(int)settings::DisplayMode::windowed};
std::atomic<int> g_request{-1};
/* The windowed mode's client rectangle (kept while full screen). */
Rect g_windowRect;
/* The windowed place changed (moved or resized) at this tick: remembered
 * once it has stopped (Windows remembers it at the end of a drag). */
uint64_t g_placeChanged;
/* The display of the full-screen modes (set_full_monitor; 0: the window's). */
std::atomic<SDL_DisplayID> g_fullDisplay{0};
/* The screen windows (create_screen), main thread only. */
std::vector<SDL_Window *> g_screens;

settings::DisplayMode cur_mode()
{
    return (settings::DisplayMode)g_mode.load();
}

/* Runs f on the main thread (now if this is it); wait: until it has run. */
template <class Fn>
void on_main(Fn &&fn, bool wait = true)
{
    using F = std::decay_t<Fn>;
    F f(std::forward<Fn>(fn));
    if (SDL_IsMainThread()) {
        f();
        return;
    }
    if (wait) {
        auto call = [](void *p) { (*static_cast<F *>(p))(); };
        SDL_RunOnMainThread(call, &f, true);
    } else {
        F *copy = new F(f);
        auto call = [](void *p) {
            F *g = static_cast<F *>(p);
            (*g)();
            delete g;
        };
        if (!SDL_RunOnMainThread(call, copy, false))
            delete copy;
    }
}

/* ---- the keys ------------------------------------------------------------ */

/* The virtual key of each scancode (USB HID usage) of a US keyboard. */
struct KeyTable {
    uint8_t vk[SDL_SCANCODE_COUNT] = {};
    KeyTable();
};

const KeyTable &key_table()
{
    static const KeyTable table;        /* (made once, on the first thread that asks) */
    return table;
}

KeyTable::KeyTable()
{
    uint8_t *t = vk;
    for (int i = 0; i < 26; i++)
        t[SDL_SCANCODE_A + i] = (uint8_t)('A' + i);
    for (int i = 0; i < 9; i++)
        t[SDL_SCANCODE_1 + i] = (uint8_t)('1' + i);
    t[SDL_SCANCODE_0] = '0';
    t[SDL_SCANCODE_RETURN] = VK_RETURN;
    t[SDL_SCANCODE_ESCAPE] = VK_ESCAPE;
    t[SDL_SCANCODE_BACKSPACE] = VK_BACK;
    t[SDL_SCANCODE_TAB] = VK_TAB;
    t[SDL_SCANCODE_SPACE] = VK_SPACE;
    t[SDL_SCANCODE_MINUS] = VK_OEM_MINUS;
    t[SDL_SCANCODE_EQUALS] = VK_OEM_PLUS;
    t[SDL_SCANCODE_LEFTBRACKET] = VK_OEM_4;
    t[SDL_SCANCODE_RIGHTBRACKET] = VK_OEM_6;
    t[SDL_SCANCODE_BACKSLASH] = VK_OEM_5;
    t[SDL_SCANCODE_NONUSHASH] = VK_OEM_5;
    t[SDL_SCANCODE_SEMICOLON] = VK_OEM_1;
    t[SDL_SCANCODE_APOSTROPHE] = VK_OEM_7;
    t[SDL_SCANCODE_GRAVE] = VK_OEM_3;
    t[SDL_SCANCODE_COMMA] = VK_OEM_COMMA;
    t[SDL_SCANCODE_PERIOD] = VK_OEM_PERIOD;
    t[SDL_SCANCODE_SLASH] = VK_OEM_2;
    t[SDL_SCANCODE_CAPSLOCK] = VK_CAPITAL;
    for (int i = 0; i < 12; i++)
        t[SDL_SCANCODE_F1 + i] = (uint8_t)(VK_F1 + i);
    for (int i = 0; i < 12; i++)
        t[SDL_SCANCODE_F13 + i] = (uint8_t)(VK_F13 + i);
    t[SDL_SCANCODE_PRINTSCREEN] = VK_SNAPSHOT;
    t[SDL_SCANCODE_SCROLLLOCK] = VK_SCROLL;
    t[SDL_SCANCODE_PAUSE] = VK_PAUSE;
    t[SDL_SCANCODE_INSERT] = VK_INSERT;
    t[SDL_SCANCODE_HOME] = VK_HOME;
    t[SDL_SCANCODE_PAGEUP] = VK_PRIOR;
    t[SDL_SCANCODE_DELETE] = VK_DELETE;
    t[SDL_SCANCODE_END] = VK_END;
    t[SDL_SCANCODE_PAGEDOWN] = VK_NEXT;
    t[SDL_SCANCODE_RIGHT] = VK_RIGHT;
    t[SDL_SCANCODE_LEFT] = VK_LEFT;
    t[SDL_SCANCODE_DOWN] = VK_DOWN;
    t[SDL_SCANCODE_UP] = VK_UP;
    t[SDL_SCANCODE_NUMLOCKCLEAR] = VK_NUMLOCK;
    t[SDL_SCANCODE_KP_DIVIDE] = VK_DIVIDE;
    t[SDL_SCANCODE_KP_MULTIPLY] = VK_MULTIPLY;
    t[SDL_SCANCODE_KP_MINUS] = VK_SUBTRACT;
    t[SDL_SCANCODE_KP_PLUS] = VK_ADD;
    t[SDL_SCANCODE_KP_ENTER] = VK_RETURN;       /* (Windows: Enter's, an extended key) */
    for (int i = 0; i < 9; i++)
        t[SDL_SCANCODE_KP_1 + i] = (uint8_t)(VK_NUMPAD1 + i);
    t[SDL_SCANCODE_KP_0] = VK_NUMPAD0;
    t[SDL_SCANCODE_KP_PERIOD] = VK_DECIMAL;
    t[SDL_SCANCODE_KP_COMMA] = VK_SEPARATOR;
    t[SDL_SCANCODE_NONUSBACKSLASH] = VK_OEM_102;
    t[SDL_SCANCODE_APPLICATION] = VK_APPS;
    t[SDL_SCANCODE_LCTRL] = VK_LCONTROL;
    t[SDL_SCANCODE_LSHIFT] = VK_LSHIFT;
    t[SDL_SCANCODE_LALT] = VK_LMENU;
    t[SDL_SCANCODE_LGUI] = VK_LWIN;
    t[SDL_SCANCODE_RCTRL] = VK_RCONTROL;
    t[SDL_SCANCODE_RSHIFT] = VK_RSHIFT;
    t[SDL_SCANCODE_RALT] = VK_RMENU;
    t[SDL_SCANCODE_RGUI] = VK_RWIN;
}

/* Num Lock off: the keypad's keys are the keys printed under their digits,
 * as Windows has them. */
int keypad_navigation(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_KP_7: return VK_HOME;
    case SDL_SCANCODE_KP_8: return VK_UP;
    case SDL_SCANCODE_KP_9: return VK_PRIOR;
    case SDL_SCANCODE_KP_4: return VK_LEFT;
    case SDL_SCANCODE_KP_5: return VK_CLEAR;
    case SDL_SCANCODE_KP_6: return VK_RIGHT;
    case SDL_SCANCODE_KP_1: return VK_END;
    case SDL_SCANCODE_KP_2: return VK_DOWN;
    case SDL_SCANCODE_KP_3: return VK_NEXT;
    case SDL_SCANCODE_KP_0: return VK_INSERT;
    case SDL_SCANCODE_KP_PERIOD: return VK_DELETE;
    default: return 0;
    }
}

/* The key's virtual key (0: none): a letter's or a digit's by the keycode
 * (the layout's), the rest by the scancode. */
int vk_of_key(SDL_Scancode sc, SDL_Keycode kc, SDL_Keymod mod)
{
    if (kc >= 'a' && kc <= 'z')
        return (int)(kc - 'a' + 'A');
    if (kc >= '0' && kc <= '9')
        return (int)kc;
    if (!(mod & SDL_KMOD_NUM))
        if (const int nav = keypad_navigation(sc))
            return nav;
    return sc >= 0 && sc < SDL_SCANCODE_COUNT ? key_table().vk[sc] : 0;
}

/* The generic virtual key of a side's modifier (Windows' messages carry
 * that one); vk otherwise. */
int generic_vk(int vk)
{
    switch (vk) {
    case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU: case VK_RMENU: return VK_MENU;
    default: return vk;
    }
}

/* The other side of a side's modifier (0: none). */
int other_side(int vk)
{
    switch (vk) {
    case VK_LSHIFT: return VK_RSHIFT;
    case VK_RSHIFT: return VK_LSHIFT;
    case VK_LCONTROL: return VK_RCONTROL;
    case VK_RCONTROL: return VK_LCONTROL;
    case VK_LMENU: return VK_RMENU;
    case VK_RMENU: return VK_LMENU;
    default: return 0;
    }
}

/* ---- the place --------------------------------------------------------- */

SDL_DisplayID window_display()
{
    SDL_DisplayID d = g_win ? SDL_GetDisplayForWindow(g_win) : 0;
    return d != 0 ? d : SDL_GetPrimaryDisplay();
}

/* The display of the full-screen modes: set_full_monitor's, else the
 * window's. */
SDL_DisplayID full_display()
{
    const SDL_DisplayID fd = g_fullDisplay.load();
    if (fd != 0) {
        SDL_Rect r;
        if (SDL_GetDisplayBounds(fd, &r))
            return fd;
    }
    return window_display();
}

/* The windowed rectangle from the settings: the client size (shrunk at 4:3
 * to fit the usable area), at its remembered place if that is on a display,
 * else centred on the primary one. */
Rect windowed_rect()
{
    const settings::Display &s = settings::display();
    int w = s.windowW, h = s.windowH;
    SDL_DisplayID home = 0;
    if (s.windowPlaced) {
        const SDL_Rect place = {s.windowX, s.windowY, w, h};
        int n = 0;
        SDL_DisplayID *ids = SDL_GetDisplays(&n);
        for (int i = 0; ids != nullptr && i < n && home == 0; i++) {
            SDL_Rect b, x;
            if (SDL_GetDisplayBounds(ids[i], &b) && SDL_GetRectIntersection(&place, &b, &x))
                home = ids[i];
        }
        SDL_free(ids);
    }
    SDL_Rect wa = {0, 0, 1280, 1024};
    if (!SDL_GetDisplayUsableBounds(home != 0 ? home : SDL_GetPrimaryDisplay(), &wa))
        SDL_GetDisplayBounds(home != 0 ? home : SDL_GetPrimaryDisplay(), &wa);
    if (w > wa.w || h > wa.h) {
        if (wa.w * 3 > wa.h * 4) {
            h = wa.h;
            w = h * 4 / 3;
        } else {
            w = wa.w;
            h = w * 3 / 4;
        }
    }
    if (home != 0)
        return Rect{s.windowX, s.windowY, w, h};
    return Rect{wa.x + (wa.w - w) / 2, wa.y + (wa.h - h) / 2, w, h};
}

void read_client_size()
{
    int w = 0, h = 0;
    if (g_win != nullptr && SDL_GetWindowSizeInPixels(g_win, &w, &h) && w > 0 && h > 0) {
        g_clientW = w;
        g_clientH = h;
    }
}

/* Remembers the windowed place (the settings too) unless the window is
 * maximized or minimized. */
void remember_windowed()
{
    g_placeChanged = 0;
    if (g_win == nullptr || cur_mode() != settings::DisplayMode::windowed)
        return;
    const SDL_WindowFlags f = SDL_GetWindowFlags(g_win);
    if (f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))
        return;
    Rect r;
    if (!SDL_GetWindowPosition(g_win, &r.x, &r.y) || !SDL_GetWindowSize(g_win, &r.w, &r.h))
        return;
    if (r.x == g_windowRect.x && r.y == g_windowRect.y && r.w == g_windowRect.w && r.h == g_windowRect.h)
        return;
    g_windowRect = r;
    settings::save_window(r.x, r.y, r.w, r.h);
}

void clear_keys()
{
    for (auto &k : g_keys)
        k = 0;
    for (auto &k : g_keyPressed)
        k = 0;
}

/* While the debug camera looks with the mouse (debug_camera::wants_mouse)
 * and the window has the focus: relative mouse mode (the cursor hidden and
 * held in the window); else let go.  No pointer over a full-screen picture
 * either. */
void update_mouse()
{
    if (g_win == nullptr)
        return;
    const bool look = debug_camera::wants_mouse();
    const bool want = g_focus && look && !g_minimized;
    if (want != g_relative) {
        SDL_SetWindowRelativeMouseMode(g_win, want);
        g_relative = want;
    }
    const bool hide = cur_mode() != settings::DisplayMode::windowed || look;
    if (hide != g_cursorHidden) {
        if (hide)
            SDL_HideCursor();
        else
            SDL_ShowCursor();
        g_cursorHidden = hide;
    }
}

/* Gives the window the place of mode m (main thread). */
void apply_mode_now(settings::DisplayMode m, bool remember)
{
    if (g_win == nullptr)
        return;
    if (remember && cur_mode() == settings::DisplayMode::windowed && m != settings::DisplayMode::windowed)
        remember_windowed();
    g_mode = (int)m;
    const SDL_WindowFlags f = SDL_GetWindowFlags(g_win);
    if (f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED))
        SDL_RestoreWindow(g_win);
    if (m == settings::DisplayMode::windowed) {
        SDL_SetWindowFullscreen(g_win, false);
        SDL_SetWindowFullscreenMode(g_win, nullptr);
        SDL_SetWindowBordered(g_win, true);
        SDL_SetWindowResizable(g_win, true);
        SDL_SetWindowSize(g_win, g_windowRect.w, g_windowRect.h);
        SDL_SetWindowPosition(g_win, g_windowRect.x, g_windowRect.y);
    } else {
        const SDL_DisplayID d = full_display();
        if (SDL_GetDisplayForWindow(g_win) != d)
            SDL_SetWindowPosition(g_win, (int)SDL_WINDOWPOS_CENTERED_DISPLAY(d), (int)SDL_WINDOWPOS_CENTERED_DISPLAY(d));
        bool set = false;
        if (m == settings::DisplayMode::fullscreen) {
            /* exclusive: the display mode of the settings (0: the desktop's
             * size; the refresh rate nearest to 60 Hz) */
            const settings::Display &s = settings::display();
            const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(d);
            const int w = s.width > 0 ? s.width : (desk ? desk->w : 0);
            const int h = s.height > 0 ? s.height : (desk ? desk->h : 0);
            const float hz = s.refresh > 0 ? (float)s.refresh : 60.0f;
            SDL_DisplayMode dm;
            if (w > 0 && h > 0 && SDL_GetClosestFullscreenDisplayMode(d, w, h, hz, false, &dm))
                set = SDL_SetWindowFullscreenMode(g_win, &dm);
            if (!set)
                pclog::write("window: no display mode %dx%d (%s): the desktop's", w, h, SDL_GetError());
        }
        if (!set)
            SDL_SetWindowFullscreenMode(g_win, nullptr);     /* borderless: the desktop's */
        SDL_SetWindowFullscreen(g_win, true);
        SDL_SyncWindow(g_win);
        if (!(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN)) {
            /* not made full screen (X11 without a window manager): as the
             * Windows window, a borderless one over the display */
            SDL_Rect r;
            if (SDL_GetDisplayBounds(d, &r)) {
                pclog::write("window: not made full screen: a borderless window over the display");
                SDL_SetWindowFullscreen(g_win, false);
                SDL_SetWindowBordered(g_win, false);
                SDL_SetWindowResizable(g_win, false);
                SDL_SetWindowPosition(g_win, r.x, r.y);
                SDL_SetWindowSize(g_win, r.w, r.h);
            }
        }
    }
    SDL_SyncWindow(g_win);
    read_client_size();
    update_mouse();
    pclog::write("window: %s", settings::mode_name(m));
}

/* ---- the hotkeys (window.cpp's) -------------------------------------- */

void run_hotkey(int h, bool repeat)
{
    switch (h) {
    case input_map::kHotTextureMenu:
        if (!repeat) {
            system_menu::hide();
            settings_menu::hide();
            texture_menu::toggle();
        }
        break;
    case input_map::kHotTextureToggle:
        if (!repeat)
            texture_menu::replace_key();
        break;
    case input_map::kHotTextureRefresh:
        if (!repeat)
            texture_menu::refresh_key();
        break;
    case input_map::kHotScreenshot:
        if (!repeat)
            screenshot::request();
        break;
    case input_map::kHotSettingsMenu:
        if (!repeat) {
            texture_menu::hide();
            system_menu::hide();
            settings_menu::toggle();
            clear_keys();
        }
        break;
    case input_map::kHotPause:
        frame_step::key_step();
        break;
    case input_map::kHotResume:
        if (!repeat)
            frame_step::key_resume();
        break;
    case input_map::kHotFullscreen:
        if (!repeat) {
            settings::DisplayMode to = settings::DisplayMode::windowed;
            if (cur_mode() == settings::DisplayMode::windowed) {
                to = settings::display().mode;
                if (to == settings::DisplayMode::windowed)
                    to = settings::DisplayMode::borderless;
            }
            g_request = (int)to;
        }
        break;
    case input_map::kHotFrameMonitor:
        if (!repeat)
            frame_monitor::toggle();
        break;
    case input_map::kHotDebugCamera:
        if (!repeat) {
            debug_camera::key();
            update_mouse();
        }
        break;
    case input_map::kHotFlickerCapture:
    case input_map::kHotTrace:
    case input_map::kHotFrameCapture:
        if (!repeat)
            g_hotPressed[h] = 1;
        break;
    case input_map::kHotSystemMenu:
        if (!repeat) {
            system_menu::toggle();
            clear_keys();
        }
        break;
    case input_map::kHotSongMenu:
        if (!repeat)
            song_osd::toggle();
        break;
    case input_map::kHotPreviousSong:
    case input_map::kHotNextSong:
        if (!repeat)
            song_osd::step(h == input_map::kHotNextSong ? 1 : -1);
        break;
    case input_map::kHotCalibrationUp:
    case input_map::kHotCalibrationDown:
        if (!repeat)
            cockpit_osd::axis_key(h == input_map::kHotCalibrationDown ? 1 : -1);
        break;
    case input_map::kHotCalibrationLeft:
    case input_map::kHotCalibrationRight:
        cockpit_osd::adjust_key(h == input_map::kHotCalibrationRight ? 1 : -1);
        break;
    case input_map::kHotCalibrationSave:
    case input_map::kHotCalibrationCancel:
    case input_map::kHotCalibrationReset:
        if (!repeat) {
            if (h == input_map::kHotCalibrationSave)
                cockpit_osd::save_key();
            else if (h == input_map::kHotCalibrationCancel)
                cockpit_osd::cancel_key();
            else
                cockpit_osd::reset_key();
        }
        break;
    }
}

bool panel_hotkey(int h)
{
    return h == input_map::kHotSongMenu || h == input_map::kHotPreviousSong || h == input_map::kHotNextSong ||
           input_map::hotkey_shares_keys(h);
}

/* A key going down (window.cpp's WM_KEYDOWN / WM_SYSKEYDOWN): the hotkeys,
 * the menus' keys, then the game's.  sys: what Windows sends as a system
 * key (Alt held, or F10), which the menus do not take. */
void key_down_event(int vk, int side, bool repeat, SDL_Keymod mod)
{
    const bool ctrl = (mod & SDL_KMOD_CTRL) != 0, alt = (mod & SDL_KMOD_ALT) != 0,
               shift = (mod & SDL_KMOD_SHIFT) != 0;
    const bool sys = alt || vk == VK_F10;
    auto hot = [&](int h) { return input_map::is_hotkey(h, vk, ctrl, alt, shift); };
    static const int kFirst[] = {input_map::kHotSettingsMenu, input_map::kHotPause, input_map::kHotResume,
                                 input_map::kHotFullscreen, input_map::kHotFrameMonitor,
                                 input_map::kHotDebugCamera, input_map::kHotFlickerCapture,
                                 input_map::kHotTrace, input_map::kHotFrameCapture,
                                 input_map::kHotTextureMenu, input_map::kHotTextureRefresh,
                                 input_map::kHotTextureToggle, input_map::kHotScreenshot};
    for (int h : kFirst)
        if (hot(h)) {
            run_hotkey(h, repeat);
            return;
        }
    if (texture_menu::shown() && !sys && texture_menu::key(vk, repeat))
        return;
    if (settings_menu::shown() && !sys) {
        settings_menu::key(vk, repeat);
        return;
    }
    if (hot(input_map::kHotSystemMenu)) {
        run_hotkey(input_map::kHotSystemMenu, repeat);
        return;
    }
    if (system_menu::shown() && !sys) {
        system_menu::key(vk, repeat);
        return;
    }
    for (int h = 0; h < input_map::kHotkeyCount; h++) {
        if (!panel_hotkey(h) || !hot(h))
            continue;
        if (input_map::hotkey_shares_keys(h) && !cockpit_osd::active())
            break;
        run_hotkey(h, repeat);
        return;
    }
    g_keys[vk] = 1;
    g_keyPressed[vk] = 1;
    if (side != 0) {
        g_keys[side] = 1;
        g_keyPressed[side] = 1;
    }
}

void key_up_event(int vk, int side)
{
    if (side != 0) {
        g_keys[side] = 0;
        /* the generic one stays down while the other side's is */
        if (g_keys[other_side(side)] == 0)
            g_keys[vk] = 0;
    } else {
        g_keys[vk] = 0;
    }
}

int mouse_vk(Uint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return VK_LBUTTON;
    case SDL_BUTTON_RIGHT: return VK_RBUTTON;
    case SDL_BUTTON_MIDDLE: return VK_MBUTTON;
    case SDL_BUTTON_X1: return VK_XBUTTON1;
    case SDL_BUTTON_X2: return VK_XBUTTON2;
    default: return 0;
    }
}

/* PC: a device's press (press_hotkey) as the key's (window.cpp's
 * kMsgHotkey). */
void device_hotkey(int h)
{
    if (panel_hotkey(h) && (settings_menu::shown() || system_menu::shown()))
        return;
    if (h == input_map::kHotSystemMenu)
        settings_menu::hide();
    run_hotkey(h, false);
}

void close_window()
{
    if (g_closing)
        return;
    remember_windowed();
    g_closing = true;
    if (g_win != nullptr)
        SDL_HideWindow(g_win);      /* (not destroyed: the swap chain may still present to it) */
}

/* One event; false: the loop ends. */
bool handle_event(const SDL_Event &e)
{
    const SDL_WindowID mainId = g_win ? SDL_GetWindowID(g_win) : 0;
    switch (e.type) {
    case SDL_EVENT_QUIT:
        close_window();
        return false;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e.window.windowID == mainId) {
            close_window();
            return false;
        }
        return true;                    /* (the screen windows: the game closes them) */
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED:
        gamepad::sdl_event(e);          /* the controllers are opened and closed here */
        return true;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (e.window.windowID == mainId) {
            g_clientW = e.window.data1;
            g_clientH = e.window.data2;
        }
        return true;
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
        if (e.window.windowID == mainId && cur_mode() == settings::DisplayMode::windowed)
            g_placeChanged = SDL_GetTicks();
        return true;
    case SDL_EVENT_WINDOW_MINIMIZED:
        if (e.window.windowID == mainId) {
            g_minimized = true;
            update_mouse();
        }
        return true;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        if (e.window.windowID == mainId) {
            g_minimized = false;
            update_mouse();
        }
        return true;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (e.window.windowID == mainId) {
            g_focus = true;
        } else if (!g_focus && g_win != nullptr) {
            /* a screen window took the focus: the keyboard back to the game
             * window */
            SDL_RaiseWindow(g_win);
        }
        update_mouse();
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (e.window.windowID == mainId) {
            g_focus = false;
            clear_keys();
            update_mouse();
        }
        return true;
    case SDL_EVENT_KEY_DOWN: {
        if (e.key.windowID != mainId)
            return true;
        const int raw = vk_of_key(e.key.scancode, e.key.key, e.key.mod);
        if (raw <= 0 || raw >= 256)
            return true;
        const int vk = generic_vk(raw);
        key_down_event(vk, vk != raw ? raw : 0, e.key.repeat, e.key.mod);
        return true;
    }
    case SDL_EVENT_KEY_UP: {
        const int raw = vk_of_key(e.key.scancode, e.key.key, e.key.mod);
        if (raw <= 0 || raw >= 256)
            return true;
        const int vk = generic_vk(raw);
        key_up_event(vk, vk != raw ? raw : 0);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (e.button.windowID != mainId)
            return true;
        const int vk = mouse_vk(e.button.button);
        if (vk == 0)
            return true;
        if (e.button.down) {
            /* the click that activates the window is not a press */
            if (!g_focus)
                return true;
            g_keys[vk] = 1;
            g_keyPressed[vk] = 1;
        } else {
            g_keys[vk] = 0;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (e.wheel.windowID != mainId)
            return true;
        int notches = e.wheel.integer_y;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            notches = -notches;
        if (notches > 0)
            g_wheelUp += (uint32_t)notches;
        else if (notches < 0)
            g_wheelDown += (uint32_t)-notches;
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.windowID == mainId && g_focus) {
            /* (relative mode: the raw movement, not held at the edges) */
            g_motionRestX += e.motion.xrel;
            g_motionRestY += e.motion.yrel;
            const int dx = (int)g_motionRestX, dy = (int)g_motionRestY;
            g_motionRestX -= (float)dx;
            g_motionRestY -= (float)dy;
            g_motionX += (uint32_t)dx;
            g_motionY += (uint32_t)dy;
        }
        return true;
    default:
        return true;
    }
}

/* The icon: the largest picture of gt3pc.ico (PNG or 32-bit DIB), the
 * others its alternates. */
#ifdef GT3PC_WINDOW_ICON
SDL_Surface *icon_image(const uint8_t *p, size_t n)
{
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (n >= 8 && memcmp(p, "\x89PNG", 4) == 0) {
        if (!png_image::decode(p, n, &rgba, &w, &h))
            return nullptr;
    } else if (n >= 40) {
        auto u32 = [&](size_t o) { return (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 |
                                          (uint32_t)p[o + 3] << 24; };
        const uint32_t hdr = u32(0);
        w = (int)u32(4);
        h = (int)u32(8) / 2;            /* (the colours and the mask) */
        const int bpp = p[14] | p[15] << 8;
        if (bpp != 32 || w <= 0 || h <= 0 || w > 256 || h > 256 || hdr + (size_t)w * h * 4 > n)
            return nullptr;
        rgba.resize((size_t)w * h * 4);
        for (int y = 0; y < h; y++) {
            const uint8_t *src = p + hdr + (size_t)(h - 1 - y) * w * 4;     /* bottom up, BGRA */
            uint8_t *dst = &rgba[(size_t)y * w * 4];
            for (int x = 0; x < w; x++) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        }
    } else {
        return nullptr;
    }
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (s == nullptr)
        return nullptr;
    for (int y = 0; y < h; y++)
        memcpy((uint8_t *)s->pixels + (size_t)y * s->pitch, &rgba[(size_t)y * w * 4], (size_t)w * 4);
    return s;
}

void set_icon()
{
    const uint8_t *ico = gt3pc_window_icon;
    const size_t n = (size_t)(gt3pc_window_icon_end - gt3pc_window_icon);
    if (n < 6 || ico[2] != 1 || ico[3] != 0)
        return;
    const int count = ico[4] | ico[5] << 8;
    std::vector<SDL_Surface *> images;
    for (int i = 0; i < count && 6 + (size_t)(i + 1) * 16 <= n; i++) {
        const uint8_t *e = ico + 6 + i * 16;
        const uint32_t size = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
        const uint32_t off = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
        if (off >= n || size > n - off)
            continue;
        if (SDL_Surface *s = icon_image(ico + off, size))
            images.push_back(s);
    }
    if (images.empty()) {
        pclog::write("window: no icon");
        return;
    }
    size_t big = 0;
    for (size_t i = 1; i < images.size(); i++)
        if (images[i]->w > images[big]->w)
            big = i;
    for (size_t i = 0; i < images.size(); i++)
        if (i != big)
            SDL_AddSurfaceAlternateImage(images[big], images[i]);
    if (!SDL_SetWindowIcon(g_win, images[big]))
        pclog::write("window: the icon was not set (%s)", SDL_GetError());
    for (SDL_Surface *s : images)
        SDL_DestroySurface(s);
}
#else
void set_icon()
{
}
#endif

} // namespace

bool create(const char *title)
{
    key_table();
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        pclog::write("window: SDL video: %s", SDL_GetError());
        return false;
    }
    const settings::DisplayMode m = settings::display().mode;
    g_windowRect = windowed_rect();
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, g_windowRect.x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, g_windowRect.y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, g_windowRect.w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, g_windowRect.h);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    /* the swap chain's (DXVK: a Vulkan surface); without a Vulkan loader the
     * window still opens */
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    g_win = SDL_CreateWindowWithProperties(props);
    if (g_win == nullptr) {
        pclog::write("window: no Vulkan window (%s)", SDL_GetError());
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, false);
        g_win = SDL_CreateWindowWithProperties(props);
    }
    SDL_DestroyProperties(props);
    if (g_win == nullptr) {
        pclog::write("window: %s", SDL_GetError());
        return false;
    }
    set_icon();
    g_mode = (int)settings::DisplayMode::windowed;
    if (m != settings::DisplayMode::windowed)
        apply_mode_now(m, false);
    SDL_ShowWindow(g_win);
    SDL_RaiseWindow(g_win);
    SDL_SyncWindow(g_win);
    read_client_size();
    update_mouse();
    pclog::write("window: %s, client %dx%d (SDL %s)", settings::mode_name(m), g_clientW.load(), g_clientH.load(),
                 SDL_GetCurrentVideoDriver());
    return true;
}

HWND handle()
{
    return g_win;
}

int run_message_loop()
{
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            do {
                if (!handle_event(e))
                    return 0;
            } while (SDL_PollEvent(&e));
        }
        /* the debug camera's look may have started or stopped meanwhile */
        update_mouse();
        if (g_placeChanged != 0 && SDL_GetTicks() - g_placeChanged > 500)
            remember_windowed();
    }
}

void client_size(int *w, int *h)
{
    *w = g_clientW;
    *h = g_clientH;
}

bool closing()
{
    return g_closing;
}

bool key_down(int vk)
{
    return vk >= 0 && vk < 256 && g_focus && g_keys[vk] != 0;
}

bool take_key_press(int vk)
{
    return vk >= 0 && vk < 256 && g_keyPressed[vk].exchange(0) != 0 && g_focus;
}

bool take_hotkey(int h)
{
    return h >= 0 && h < input_map::kHotkeyCount && g_hotPressed[h].exchange(0) != 0 && g_focus;
}

void press_hotkey(int h)
{
    if (g_win != nullptr && h >= 0 && h < input_map::kHotkeyCount)
        on_main([h] { device_hotkey(h); }, false);
}

void mouse_wheel(uint32_t *up, uint32_t *down)
{
    *up = g_wheelUp.load();
    *down = g_wheelDown.load();
}

void mouse_motion(uint32_t *x, uint32_t *y)
{
    *x = g_motionX.load();
    *y = g_motionY.load();
}

settings::DisplayMode mode()
{
    return cur_mode();
}

bool take_mode_request(settings::DisplayMode *m)
{
    int r = g_request.exchange(-1);
    if (r < 0)
        return false;
    *m = (settings::DisplayMode)r;
    return true;
}

void request_mode(settings::DisplayMode m)
{
    g_request = (int)m;
}

void apply_mode(settings::DisplayMode m)
{
    on_main([m] { apply_mode_now(m, true); });
}

bool active()
{
    return g_focus && g_win != nullptr && !g_minimized;
}

HWND create_screen(int x, int y, int w, int h)
{
    SDL_Window *made = nullptr;
    on_main([&] {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Gran Turismo 3 A-Spec");
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_UTILITY_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
        made = SDL_CreateWindowWithProperties(props);
        if (made == nullptr) {
            SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, false);
            made = SDL_CreateWindowWithProperties(props);
        }
        SDL_DestroyProperties(props);
        if (made != nullptr) {
            g_screens.push_back(made);
            if (g_cursorHidden)
                SDL_HideCursor();
        } else {
            pclog::write("window: no screen window (%s)", SDL_GetError());
        }
    });
    return made;
}

void destroy_screen(HWND hwnd)
{
    if (hwnd == nullptr)
        return;
    on_main([hwnd] {
        for (size_t i = 0; i < g_screens.size(); i++)
            if (g_screens[i] == (SDL_Window *)hwnd) {
                g_screens.erase(g_screens.begin() + (long)i);
                SDL_DestroyWindow((SDL_Window *)hwnd);
                return;
            }
    });
}

void set_full_monitor(void *monitor)
{
    /* off Windows the monitor is an SDL_DisplayID */
    g_fullDisplay = (SDL_DisplayID)(uintptr_t)monitor;
}

void activate()
{
    on_main(
        [] {
            /* only while the game has the keyboard (its own screen took it) */
            SDL_Window *f = SDL_GetKeyboardFocus();
            if (g_win != nullptr && !g_focus && f != nullptr && f != g_win)
                SDL_RaiseWindow(g_win);
        },
        false);
}

int sdl_scancode(int vk)
{
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:
        return 0;                       /* (either side's: no one key) */
    case VK_RETURN:
        return SDL_SCANCODE_RETURN;
    case VK_OEM_5:
        return SDL_SCANCODE_BACKSLASH;
    }
    if (vk <= 0 || vk >= 256)
        return 0;
    const KeyTable &t = key_table();
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++)
        if (t.vk[sc] == vk)
            return sc;
    return 0;
}

} // namespace window
