/* window.h - the game window of GT3 PC.
 *
 * One Win32 window, created and pumped by the program's first thread (the
 * game itself runs on its own threads).  It shows the game at 4:3 in its
 * client area, in the display mode of gt3pc.ini (platform/settings.h):
 *   windowed     a resizable window (its size and place are remembered);
 *   borderless   a borderless window covering its monitor;
 *   fullscreen   the same, the GPU module then takes the monitor's display
 *                mode (DXGI exclusive fullscreen, gpu.cpp).
 * Alt+Enter (the Fullscreen hotkey: input_map.h [Hotkeys]) switches between
 * windowed and the full-screen mode of the settings (borderless when the
 * settings say windowed); the settings menu's SCREEN MODE sets the mode of
 * the settings and switches to it.  The window thread
 * only records that request: the swap chain belongs to the game thread, so
 * the GPU module applies it (take_mode_request, apply_mode).
 *
 * Off Windows the window is SDL3's (platform/sdl/window_sdl.cpp), with the
 * same calls: its keys translated to the Windows virtual keys (what
 * gt3pc.ini stores), exclusive fullscreen by the display mode SDL sets, and
 * the calls of other threads run on the window's (SDL's main thread). */
#pragma once

#include <stdint.h>

#include "platform/settings.h"

#ifdef _WIN32
struct HWND__;
typedef HWND__ *HWND;
#else
/* Off Windows the window is SDL3's (platform/sdl/window_sdl.cpp) and HWND
 * its opaque handle, the SDL_Window *: the same type DXVK-native's
 * windows_base.h gives HWND, so both headers can be included together. */
typedef void *HWND;
#endif

namespace window {

bool create(const char *title);
HWND handle();
/* Runs the message loop until the window closes; returns the exit code. */
int  run_message_loop();
/* Client area size (changes when the window is resized or its mode changes). */
void client_size(int *w, int *h);
bool closing();

/* Keyboard state for the pad backend: true while the virtual key is down
 * and the window has the focus.  The mouse's buttons are keys too
 * (VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1 / 2: over the window's
 * picture; the click that activates the window is not one). */
bool key_down(int vk);
/* The key went down since the last call (a tap between two of the game's
 * reads; platform/pad_pc.h), and the window has the focus. */
bool take_key_press(int vk);
/* Hotkey h (input_map.h; the debug captures': FlickerCapture, Trace) went
 * down since the last call, and the window has the focus.  The other
 * hotkeys the window thread acts on itself. */
bool take_hotkey(int h);
/* PC: a device's press of hotkey h (its [Hotkeys] binding after `;',
 * gamepad.cpp's thread): the window thread acts on it as on its key. */
void press_hotkey(int h);
/* The mouse wheel's notches up and down over the window so far (counts
 * that only grow: a reader compares them with the last it saw). */
void mouse_wheel(uint32_t *up, uint32_t *down);
/* The mouse's raw movement while the window has the focus, summed (counts,
 * x right +, y down +; wrapping sums: a reader takes the difference with
 * the last it saw, as int32_t).  The debug camera's look (debug_camera.h);
 * meanwhile the cursor is hidden and confined to the window. */
void mouse_motion(uint32_t *x, uint32_t *y);

/* The display mode the window is in. */
settings::DisplayMode mode();
/* A mode change the player asked for (Alt+Enter, request_mode), once;
 * false if none. */
bool take_mode_request(settings::DisplayMode *m);
/* Asks for mode m (the settings menu's SCREEN MODE; any thread): the GPU
 * module applies it as Alt+Enter's. */
void request_mode(settings::DisplayMode m);
/* Gives the window the style and place of mode m: windowed, its remembered
 * place; borderless / fullscreen, its monitor.  Called by the GPU module. */
void apply_mode(settings::DisplayMode m);
/* The window has the focus and is not minimized. */
bool active();

/* Multi-monitor (gpu.cpp's extra screens: the span, the split screen's
 * second monitor).  A screen window: borderless over the rectangle, owned
 * by the game window, never activated (the game window keeps the keyboard);
 * made and destroyed on the window thread (these block until it has). */
HWND create_screen(int x, int y, int w, int h);
void destroy_screen(HWND hwnd);
/* The monitor (an HMONITOR) the borderless / fullscreen modes cover (the
 * span's monitor of the game window); nullptr: the window's own.  Applied
 * by the next apply_mode. */
void set_full_monitor(void *monitor);
/* Gives the game window the focus again (a screen window's exclusive
 * fullscreen took it). */
void activate();

#ifndef _WIN32
/* Off Windows: the SDL scancode (SDL_Scancode) of virtual key vk, the key
 * at that place of a US keyboard (0: none; the mouse's buttons have none).
 * The keys' names in the keyboard's layout (input_map::describe). */
int sdl_scancode(int vk);
#endif

} // namespace window
